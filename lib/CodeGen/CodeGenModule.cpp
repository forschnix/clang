//===--- CodeGenModule.cpp - Emit LLVM Code from ASTs for a Module --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This coordinates the per-module state used while generating code.
//
//===----------------------------------------------------------------------===//

#include "CodeGenModule.h"
#include "CGDebugInfo.h"
#include "CodeGenFunction.h"
#include "CodeGenTBAA.h"
#include "CGCall.h"
#include "CGCUDARuntime.h"
#include "CGCXXABI.h"
#include "CGObjCRuntime.h"
#include "CGOpenCLRuntime.h"
#include "TargetInfo.h"
#include "clang/Frontend/CodeGenOptions.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/CharUnits.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/ConvertUTF.h"
#include "llvm/CallingConv.h"
#include "llvm/Module.h"
#include "llvm/Intrinsics.h"
#include "llvm/LLVMContext.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Target/Mangler.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/ErrorHandling.h"
using namespace clang;
using namespace CodeGen;

static const char AnnotationSection[] = "llvm.metadata";

static CGCXXABI &createCXXABI(CodeGenModule &CGM) {
  switch (CGM.getContext().getTargetInfo().getCXXABI()) {
  case CXXABI_ARM: return *CreateARMCXXABI(CGM);
  case CXXABI_Itanium: return *CreateItaniumCXXABI(CGM);
  case CXXABI_Microsoft: return *CreateMicrosoftCXXABI(CGM);
  }

  llvm_unreachable("invalid C++ ABI kind");
}


CodeGenModule::CodeGenModule(ASTContext &C, const CodeGenOptions &CGO,
                             llvm::Module &M, const llvm::TargetData &TD,
                             DiagnosticsEngine &diags)
  : Context(C), LangOpts(C.getLangOpts()), CodeGenOpts(CGO), TheModule(M),
    TheTargetData(TD), TheTargetCodeGenInfo(0), Diags(diags),
    ABI(createCXXABI(*this)), 
    Types(*this),
    TBAA(0),
    VTables(*this), ObjCRuntime(0), OpenCLRuntime(0), CUDARuntime(0),
    DebugInfo(0), ARCData(0), NoObjCARCExceptionsMetadata(0),
    RRData(0), CFConstantStringClassRef(0),
    ConstantStringClassRef(0), NSConstantStringType(0),
    VMContext(M.getContext()),
    NSConcreteGlobalBlock(0), NSConcreteStackBlock(0),
    BlockObjectAssign(0), BlockObjectDispose(0),
    BlockDescriptorType(0), GenericBlockLiteralType(0) {
      
  // Initialize the type cache.
  llvm::LLVMContext &LLVMContext = M.getContext();
  VoidTy = llvm::Type::getVoidTy(LLVMContext);
  Int8Ty = llvm::Type::getInt8Ty(LLVMContext);
  Int16Ty = llvm::Type::getInt16Ty(LLVMContext);
  Int32Ty = llvm::Type::getInt32Ty(LLVMContext);
  Int64Ty = llvm::Type::getInt64Ty(LLVMContext);
  FloatTy = llvm::Type::getFloatTy(LLVMContext);
  DoubleTy = llvm::Type::getDoubleTy(LLVMContext);
  PointerWidthInBits = C.getTargetInfo().getPointerWidth(0);
  PointerAlignInBytes =
  C.toCharUnitsFromBits(C.getTargetInfo().getPointerAlign(0)).getQuantity();
  IntTy = llvm::IntegerType::get(LLVMContext, C.getTargetInfo().getIntWidth());
  IntPtrTy = llvm::IntegerType::get(LLVMContext, PointerWidthInBits);
  Int8PtrTy = Int8Ty->getPointerTo(0);
  Int8PtrPtrTy = Int8PtrTy->getPointerTo(0);

  if (LangOpts.ObjC1)
    createObjCRuntime();
  if (LangOpts.OpenCL)
    createOpenCLRuntime();
  if (LangOpts.CUDA)
    createCUDARuntime();

  // Enable TBAA unless it's suppressed. ThreadSanitizer needs TBAA even at O0.
  if (LangOpts.ThreadSanitizer ||
      (!CodeGenOpts.RelaxedAliasing && CodeGenOpts.OptimizationLevel > 0))
    TBAA = new CodeGenTBAA(Context, VMContext, CodeGenOpts, getLangOpts(),
                           ABI.getMangleContext());

  // If debug info or coverage generation is enabled, create the CGDebugInfo
  // object.
  if (CodeGenOpts.DebugInfo != CodeGenOptions::NoDebugInfo ||
      CodeGenOpts.EmitGcovArcs ||
      CodeGenOpts.EmitGcovNotes)
    DebugInfo = new CGDebugInfo(*this);

  Block.GlobalUniqueCount = 0;

  if (C.getLangOpts().ObjCAutoRefCount)
    ARCData = new ARCEntrypoints();
  RRData = new RREntrypoints();
}

CodeGenModule::~CodeGenModule() {
  delete ObjCRuntime;
  delete OpenCLRuntime;
  delete CUDARuntime;
  delete TheTargetCodeGenInfo;
  delete &ABI;
  delete TBAA;
  delete DebugInfo;
  delete ARCData;
  delete RRData;
}

void CodeGenModule::createObjCRuntime() {
  if (!LangOpts.NeXTRuntime)
    ObjCRuntime = CreateGNUObjCRuntime(*this);
  else
    ObjCRuntime = CreateMacObjCRuntime(*this);
}

void CodeGenModule::createOpenCLRuntime() {
  OpenCLRuntime = new CGOpenCLRuntime(*this);
}

void CodeGenModule::createCUDARuntime() {
  CUDARuntime = CreateNVCUDARuntime(*this);
}

void CodeGenModule::Release() {
  EmitDeferred();
  EmitCXXGlobalInitFunc();
  EmitCXXGlobalDtorFunc();
  if (ObjCRuntime)
    if (llvm::Function *ObjCInitFunction = ObjCRuntime->ModuleInitFunction())
      AddGlobalCtor(ObjCInitFunction);
  EmitCtorList(GlobalCtors, "llvm.global_ctors");
  EmitCtorList(GlobalDtors, "llvm.global_dtors");
  EmitGlobalAnnotations();
  EmitLLVMUsed();

  SimplifyPersonality();

  if (getCodeGenOpts().EmitDeclMetadata)
    EmitDeclMetadata();

  if (getCodeGenOpts().EmitGcovArcs || getCodeGenOpts().EmitGcovNotes)
    EmitCoverageFile();

  if (DebugInfo)
    DebugInfo->finalize();
}

void CodeGenModule::UpdateCompletedType(const TagDecl *TD) {
  // Make sure that this type is translated.
  Types.UpdateCompletedType(TD);
}

llvm::MDNode *CodeGenModule::getTBAAInfo(QualType QTy) {
  if (!TBAA)
    return 0;
  return TBAA->getTBAAInfo(QTy);
}

llvm::MDNode *CodeGenModule::getTBAAInfoForVTablePtr() {
  if (!TBAA)
    return 0;
  return TBAA->getTBAAInfoForVTablePtr();
}

void CodeGenModule::DecorateInstruction(llvm::Instruction *Inst,
                                        llvm::MDNode *TBAAInfo) {
  Inst->setMetadata(llvm::LLVMContext::MD_tbaa, TBAAInfo);
}

bool CodeGenModule::isTargetDarwin() const {
  return getContext().getTargetInfo().getTriple().isOSDarwin();
}

void CodeGenModule::Error(SourceLocation loc, StringRef error) {
  unsigned diagID = getDiags().getCustomDiagID(DiagnosticsEngine::Error, error);
  getDiags().Report(Context.getFullLoc(loc), diagID);
}

/// ErrorUnsupported - Print out an error that codegen doesn't support the
/// specified stmt yet.
void CodeGenModule::ErrorUnsupported(const Stmt *S, const char *Type,
                                     bool OmitOnError) {
  if (OmitOnError && getDiags().hasErrorOccurred())
    return;
  unsigned DiagID = getDiags().getCustomDiagID(DiagnosticsEngine::Error,
                                               "cannot compile this %0 yet");
  std::string Msg = Type;
  getDiags().Report(Context.getFullLoc(S->getLocStart()), DiagID)
    << Msg << S->getSourceRange();
}

/// ErrorUnsupported - Print out an error that codegen doesn't support the
/// specified decl yet.
void CodeGenModule::ErrorUnsupported(const Decl *D, const char *Type,
                                     bool OmitOnError) {
  if (OmitOnError && getDiags().hasErrorOccurred())
    return;
  unsigned DiagID = getDiags().getCustomDiagID(DiagnosticsEngine::Error,
                                               "cannot compile this %0 yet");
  std::string Msg = Type;
  getDiags().Report(Context.getFullLoc(D->getLocation()), DiagID) << Msg;
}

llvm::ConstantInt *CodeGenModule::getSize(CharUnits size) {
  return llvm::ConstantInt::get(SizeTy, size.getQuantity());
}

void CodeGenModule::setGlobalVisibility(llvm::GlobalValue *GV,
                                        const NamedDecl *D) const {
  // Internal definitions always have default visibility.
  if (GV->hasLocalLinkage()) {
    GV->setVisibility(llvm::GlobalValue::DefaultVisibility);
    return;
  }

  // Set visibility for definitions.
  NamedDecl::LinkageInfo LV = D->getLinkageAndVisibility();
  if (LV.visibilityExplicit() || !GV->hasAvailableExternallyLinkage())
    GV->setVisibility(GetLLVMVisibility(LV.visibility()));
}

/// Set the symbol visibility of type information (vtable and RTTI)
/// associated with the given type.
void CodeGenModule::setTypeVisibility(llvm::GlobalValue *GV,
                                      const CXXRecordDecl *RD,
                                      TypeVisibilityKind TVK) const {
  setGlobalVisibility(GV, RD);

  if (!CodeGenOpts.HiddenWeakVTables)
    return;

  // We never want to drop the visibility for RTTI names.
  if (TVK == TVK_ForRTTIName)
    return;

  // We want to drop the visibility to hidden for weak type symbols.
  // This isn't possible if there might be unresolved references
  // elsewhere that rely on this symbol being visible.

  // This should be kept roughly in sync with setThunkVisibility
  // in CGVTables.cpp.

  // Preconditions.
  if (GV->getLinkage() != llvm::GlobalVariable::LinkOnceODRLinkage ||
      GV->getVisibility() != llvm::GlobalVariable::DefaultVisibility)
    return;

  // Don't override an explicit visibility attribute.
  if (RD->getExplicitVisibility())
    return;

  switch (RD->getTemplateSpecializationKind()) {
  // We have to disable the optimization if this is an EI definition
  // because there might be EI declarations in other shared objects.
  case TSK_ExplicitInstantiationDefinition:
  case TSK_ExplicitInstantiationDeclaration:
    return;

  // Every use of a non-template class's type information has to emit it.
  case TSK_Undeclared:
    break;

  // In theory, implicit instantiations can ignore the possibility of
  // an explicit instantiation declaration because there necessarily
  // must be an EI definition somewhere with default visibility.  In
  // practice, it's possible to have an explicit instantiation for
  // an arbitrary template class, and linkers aren't necessarily able
  // to deal with mixed-visibility symbols.
  case TSK_ExplicitSpecialization:
  case TSK_ImplicitInstantiation:
    if (!CodeGenOpts.HiddenWeakTemplateVTables)
      return;
    break;
  }

  // If there's a key function, there may be translation units
  // that don't have the key function's definition.  But ignore
  // this if we're emitting RTTI under -fno-rtti.
  if (!(TVK != TVK_ForRTTI) || LangOpts.RTTI) {
    if (Context.getKeyFunction(RD))
      return;
  }

  // Otherwise, drop the visibility to hidden.
  GV->setVisibility(llvm::GlobalValue::HiddenVisibility);
  GV->setUnnamedAddr(true);
}

StringRef CodeGenModule::getMangledName(GlobalDecl GD) {
  const NamedDecl *ND = cast<NamedDecl>(GD.getDecl());

  StringRef &Str = MangledDeclNames[GD.getCanonicalDecl()];
  if (!Str.empty())
    return Str;

  if (!getCXXABI().getMangleContext().shouldMangleDeclName(ND)) {
    IdentifierInfo *II = ND->getIdentifier();
    assert(II && "Attempt to mangle unnamed decl.");

    Str = II->getName();
    return Str;
  }
  
  SmallString<256> Buffer;
  llvm::raw_svector_ostream Out(Buffer);
  if (const CXXConstructorDecl *D = dyn_cast<CXXConstructorDecl>(ND))
    getCXXABI().getMangleContext().mangleCXXCtor(D, GD.getCtorType(), Out);
  else if (const CXXDestructorDecl *D = dyn_cast<CXXDestructorDecl>(ND))
    getCXXABI().getMangleContext().mangleCXXDtor(D, GD.getDtorType(), Out);
  else if (const BlockDecl *BD = dyn_cast<BlockDecl>(ND))
    getCXXABI().getMangleContext().mangleBlock(BD, Out);
  else
    getCXXABI().getMangleContext().mangleName(ND, Out);

  // Allocate space for the mangled name.
  Out.flush();
  size_t Length = Buffer.size();
  char *Name = MangledNamesAllocator.Allocate<char>(Length);
  std::copy(Buffer.begin(), Buffer.end(), Name);
  
  Str = StringRef(Name, Length);
  
  return Str;
}

void CodeGenModule::getBlockMangledName(GlobalDecl GD, MangleBuffer &Buffer,
                                        const BlockDecl *BD) {
  MangleContext &MangleCtx = getCXXABI().getMangleContext();
  const Decl *D = GD.getDecl();
  llvm::raw_svector_ostream Out(Buffer.getBuffer());
  if (D == 0)
    MangleCtx.mangleGlobalBlock(BD, Out);
  else if (const CXXConstructorDecl *CD = dyn_cast<CXXConstructorDecl>(D))
    MangleCtx.mangleCtorBlock(CD, GD.getCtorType(), BD, Out);
  else if (const CXXDestructorDecl *DD = dyn_cast<CXXDestructorDecl>(D))
    MangleCtx.mangleDtorBlock(DD, GD.getDtorType(), BD, Out);
  else
    MangleCtx.mangleBlock(cast<DeclContext>(D), BD, Out);
}

llvm::GlobalValue *CodeGenModule::GetGlobalValue(StringRef Name) {
  return getModule().getNamedValue(Name);
}

/// AddGlobalCtor - Add a function to the list that will be called before
/// main() runs.
void CodeGenModule::AddGlobalCtor(llvm::Function * Ctor, int Priority) {
  // FIXME: Type coercion of void()* types.
  GlobalCtors.push_back(std::make_pair(Ctor, Priority));
}

/// AddGlobalDtor - Add a function to the list that will be called
/// when the module is unloaded.
void CodeGenModule::AddGlobalDtor(llvm::Function * Dtor, int Priority) {
  // FIXME: Type coercion of void()* types.
  GlobalDtors.push_back(std::make_pair(Dtor, Priority));
}

void CodeGenModule::EmitCtorList(const CtorList &Fns, const char *GlobalName) {
  // Ctor function type is void()*.
  llvm::FunctionType* CtorFTy = llvm::FunctionType::get(VoidTy, false);
  llvm::Type *CtorPFTy = llvm::PointerType::getUnqual(CtorFTy);

  // Get the type of a ctor entry, { i32, void ()* }.
  llvm::StructType *CtorStructTy =
    llvm::StructType::get(Int32Ty, llvm::PointerType::getUnqual(CtorFTy), NULL);

  // Construct the constructor and destructor arrays.
  SmallVector<llvm::Constant*, 8> Ctors;
  for (CtorList::const_iterator I = Fns.begin(), E = Fns.end(); I != E; ++I) {
    llvm::Constant *S[] = {
      llvm::ConstantInt::get(Int32Ty, I->second, false),
      llvm::ConstantExpr::getBitCast(I->first, CtorPFTy)
    };
    Ctors.push_back(llvm::ConstantStruct::get(CtorStructTy, S));
  }

  if (!Ctors.empty()) {
    llvm::ArrayType *AT = llvm::ArrayType::get(CtorStructTy, Ctors.size());
    new llvm::GlobalVariable(TheModule, AT, false,
                             llvm::GlobalValue::AppendingLinkage,
                             llvm::ConstantArray::get(AT, Ctors),
                             GlobalName);
  }
}

llvm::GlobalValue::LinkageTypes
CodeGenModule::getFunctionLinkage(const FunctionDecl *D) {
  GVALinkage Linkage = getContext().GetGVALinkageForFunction(D);

  if (Linkage == GVA_Internal)
    return llvm::Function::InternalLinkage;
  
  if (D->hasAttr<DLLExportAttr>())
    return llvm::Function::DLLExportLinkage;
  
  if (D->hasAttr<WeakAttr>())
    return llvm::Function::WeakAnyLinkage;
  
  // In C99 mode, 'inline' functions are guaranteed to have a strong
  // definition somewhere else, so we can use available_externally linkage.
  if (Linkage == GVA_C99Inline)
    return llvm::Function::AvailableExternallyLinkage;

  // Note that Apple's kernel linker doesn't support symbol
  // coalescing, so we need to avoid linkonce and weak linkages there.
  // Normally, this means we just map to internal, but for explicit
  // instantiations we'll map to external.

  // In C++, the compiler has to emit a definition in every translation unit
  // that references the function.  We should use linkonce_odr because
  // a) if all references in this translation unit are optimized away, we
  // don't need to codegen it.  b) if the function persists, it needs to be
  // merged with other definitions. c) C++ has the ODR, so we know the
  // definition is dependable.
  if (Linkage == GVA_CXXInline || Linkage == GVA_TemplateInstantiation)
    return !Context.getLangOpts().AppleKext 
             ? llvm::Function::LinkOnceODRLinkage 
             : llvm::Function::InternalLinkage;
  
  // An explicit instantiation of a template has weak linkage, since
  // explicit instantiations can occur in multiple translation units
  // and must all be equivalent. However, we are not allowed to
  // throw away these explicit instantiations.
  if (Linkage == GVA_ExplicitTemplateInstantiation)
    return !Context.getLangOpts().AppleKext
             ? llvm::Function::WeakODRLinkage
             : llvm::Function::ExternalLinkage;
  
  // Otherwise, we have strong external linkage.
  assert(Linkage == GVA_StrongExternal);
  return llvm::Function::ExternalLinkage;
}


/// SetFunctionDefinitionAttributes - Set attributes for a global.
///
/// FIXME: This is currently only done for aliases and functions, but not for
/// variables (these details are set in EmitGlobalVarDefinition for variables).
void CodeGenModule::SetFunctionDefinitionAttributes(const FunctionDecl *D,
                                                    llvm::GlobalValue *GV) {
  SetCommonAttributes(D, GV);
}

void CodeGenModule::SetLLVMFunctionAttributes(const Decl *D,
                                              const CGFunctionInfo &Info,
                                              llvm::Function *F) {
  unsigned CallingConv;
  AttributeListType AttributeList;
  ConstructAttributeList(Info, D, AttributeList, CallingConv);
  F->setAttributes(llvm::AttrListPtr::get(AttributeList.begin(),
                                          AttributeList.size()));
  F->setCallingConv(static_cast<llvm::CallingConv::ID>(CallingConv));
}

/// Determines whether the language options require us to model
/// unwind exceptions.  We treat -fexceptions as mandating this
/// except under the fragile ObjC ABI with only ObjC exceptions
/// enabled.  This means, for example, that C with -fexceptions
/// enables this.
static bool hasUnwindExceptions(const LangOptions &LangOpts) {
  // If exceptions are completely disabled, obviously this is false.
  if (!LangOpts.Exceptions) return false;

  // If C++ exceptions are enabled, this is true.
  if (LangOpts.CXXExceptions) return true;

  // If ObjC exceptions are enabled, this depends on the ABI.
  if (LangOpts.ObjCExceptions) {
    if (!LangOpts.ObjCNonFragileABI) return false;
  }

  return true;
}

void CodeGenModule::SetLLVMFunctionAttributesForDefinition(const Decl *D,
                                                           llvm::Function *F) {
  if (CodeGenOpts.UnwindTables)
    F->setHasUWTable();

  if (!hasUnwindExceptions(LangOpts))
    F->addFnAttr(llvm::Attribute::NoUnwind);

  if (D->hasAttr<NakedAttr>()) {
    // Naked implies noinline: we should not be inlining such functions.
    F->addFnAttr(llvm::Attribute::Naked);
    F->addFnAttr(llvm::Attribute::NoInline);
  }

  if (D->hasAttr<NoInlineAttr>())
    F->addFnAttr(llvm::Attribute::NoInline);

  // (noinline wins over always_inline, and we can't specify both in IR)
  if (D->hasAttr<AlwaysInlineAttr>() &&
      !F->hasFnAttr(llvm::Attribute::NoInline))
    F->addFnAttr(llvm::Attribute::AlwaysInline);

  // FIXME: Communicate hot and cold attributes to LLVM more directly.
  if (D->hasAttr<ColdAttr>())
    F->addFnAttr(llvm::Attribute::OptimizeForSize);

  if (isa<CXXConstructorDecl>(D) || isa<CXXDestructorDecl>(D))
    F->setUnnamedAddr(true);

  if (LangOpts.getStackProtector() == LangOptions::SSPOn)
    F->addFnAttr(llvm::Attribute::StackProtect);
  else if (LangOpts.getStackProtector() == LangOptions::SSPReq)
    F->addFnAttr(llvm::Attribute::StackProtectReq);
  
  if (LangOpts.AddressSanitizer) {
    // When AddressSanitizer is enabled, set AddressSafety attribute
    // unless __attribute__((no_address_safety_analysis)) is used.
    if (!D->hasAttr<NoAddressSafetyAnalysisAttr>())
      F->addFnAttr(llvm::Attribute::AddressSafety);
  }

  unsigned alignment = D->getMaxAlignment() / Context.getCharWidth();
  if (alignment)
    F->setAlignment(alignment);

  // C++ ABI requires 2-byte alignment for member functions.
  if (F->getAlignment() < 2 && isa<CXXMethodDecl>(D))
    F->setAlignment(2);
}

void CodeGenModule::SetCommonAttributes(const Decl *D,
                                        llvm::GlobalValue *GV) {
  if (const NamedDecl *ND = dyn_cast<NamedDecl>(D))
    setGlobalVisibility(GV, ND);
  else
    GV->setVisibility(llvm::GlobalValue::DefaultVisibility);

  if (D->hasAttr<UsedAttr>())
    AddUsedGlobal(GV);

  if (const SectionAttr *SA = D->getAttr<SectionAttr>())
    GV->setSection(SA->getName());

  getTargetCodeGenInfo().SetTargetAttributes(D, GV, *this);
}

void CodeGenModule::SetInternalFunctionAttributes(const Decl *D,
                                                  llvm::Function *F,
                                                  const CGFunctionInfo &FI) {
  SetLLVMFunctionAttributes(D, FI, F);
  SetLLVMFunctionAttributesForDefinition(D, F);

  F->setLinkage(llvm::Function::InternalLinkage);

  SetCommonAttributes(D, F);
}

void CodeGenModule::SetFunctionAttributes(GlobalDecl GD,
                                          llvm::Function *F,
                                          bool IsIncompleteFunction) {
  if (unsigned IID = F->getIntrinsicID()) {
    // If this is an intrinsic function, set the function's attributes
    // to the intrinsic's attributes.
    F->setAttributes(llvm::Intrinsic::getAttributes((llvm::Intrinsic::ID)IID));
    return;
  }

  const FunctionDecl *FD = cast<FunctionDecl>(GD.getDecl());

  if (!IsIncompleteFunction)
    SetLLVMFunctionAttributes(FD, getTypes().arrangeGlobalDeclaration(GD), F);

  // Only a few attributes are set on declarations; these may later be
  // overridden by a definition.

  if (FD->hasAttr<DLLImportAttr>()) {
    F->setLinkage(llvm::Function::DLLImportLinkage);
  } else if (FD->hasAttr<WeakAttr>() ||
             FD->isWeakImported()) {
    // "extern_weak" is overloaded in LLVM; we probably should have
    // separate linkage types for this.
    F->setLinkage(llvm::Function::ExternalWeakLinkage);
  } else {
    F->setLinkage(llvm::Function::ExternalLinkage);

    NamedDecl::LinkageInfo LV = FD->getLinkageAndVisibility();
    if (LV.linkage() == ExternalLinkage && LV.visibilityExplicit()) {
      F->setVisibility(GetLLVMVisibility(LV.visibility()));
    }
  }

  if (const SectionAttr *SA = FD->getAttr<SectionAttr>())
    F->setSection(SA->getName());
}

void CodeGenModule::AddUsedGlobal(llvm::GlobalValue *GV) {
  assert(!GV->isDeclaration() &&
         "Only globals with definition can force usage.");
  LLVMUsed.push_back(GV);
}

void CodeGenModule::EmitLLVMUsed() {
  // Don't create llvm.used if there is no need.
  if (LLVMUsed.empty())
    return;

  // Convert LLVMUsed to what ConstantArray needs.
  SmallVector<llvm::Constant*, 8> UsedArray;
  UsedArray.resize(LLVMUsed.size());
  for (unsigned i = 0, e = LLVMUsed.size(); i != e; ++i) {
    UsedArray[i] =
     llvm::ConstantExpr::getBitCast(cast<llvm::Constant>(&*LLVMUsed[i]),
                                    Int8PtrTy);
  }

  if (UsedArray.empty())
    return;
  llvm::ArrayType *ATy = llvm::ArrayType::get(Int8PtrTy, UsedArray.size());

  llvm::GlobalVariable *GV =
    new llvm::GlobalVariable(getModule(), ATy, false,
                             llvm::GlobalValue::AppendingLinkage,
                             llvm::ConstantArray::get(ATy, UsedArray),
                             "llvm.used");

  GV->setSection("llvm.metadata");
}

void CodeGenModule::EmitDeferred() {
  // Emit code for any potentially referenced deferred decls.  Since a
  // previously unused static decl may become used during the generation of code
  // for a static function, iterate until no changes are made.

  while (!DeferredDeclsToEmit.empty() || !DeferredVTables.empty()) {
    if (!DeferredVTables.empty()) {
      const CXXRecordDecl *RD = DeferredVTables.back();
      DeferredVTables.pop_back();
      getVTables().GenerateClassData(getVTableLinkage(RD), RD);
      continue;
    }

    GlobalDecl D = DeferredDeclsToEmit.back();
    DeferredDeclsToEmit.pop_back();

    // Check to see if we've already emitted this.  This is necessary
    // for a couple of reasons: first, decls can end up in the
    // deferred-decls queue multiple times, and second, decls can end
    // up with definitions in unusual ways (e.g. by an extern inline
    // function acquiring a strong function redefinition).  Just
    // ignore these cases.
    //
    // TODO: That said, looking this up multiple times is very wasteful.
    StringRef Name = getMangledName(D);
    llvm::GlobalValue *CGRef = GetGlobalValue(Name);
    assert(CGRef && "Deferred decl wasn't referenced?");

    if (!CGRef->isDeclaration())
      continue;

    // GlobalAlias::isDeclaration() defers to the aliasee, but for our
    // purposes an alias counts as a definition.
    if (isa<llvm::GlobalAlias>(CGRef))
      continue;

    // Otherwise, emit the definition and move on to the next one.
    EmitGlobalDefinition(D);
  }
}

void CodeGenModule::EmitGlobalAnnotations() {
  if (Annotations.empty())
    return;

  // Create a new global variable for the ConstantStruct in the Module.
  llvm::Constant *Array = llvm::ConstantArray::get(llvm::ArrayType::get(
    Annotations[0]->getType(), Annotations.size()), Annotations);
  llvm::GlobalValue *gv = new llvm::GlobalVariable(getModule(),
    Array->getType(), false, llvm::GlobalValue::AppendingLinkage, Array,
    "llvm.global.annotations");
  gv->setSection(AnnotationSection);
}

llvm::Constant *CodeGenModule::EmitAnnotationString(llvm::StringRef Str) {
  llvm::StringMap<llvm::Constant*>::iterator i = AnnotationStrings.find(Str);
  if (i != AnnotationStrings.end())
    return i->second;

  // Not found yet, create a new global.
  llvm::Constant *s = llvm::ConstantDataArray::getString(getLLVMContext(), Str);
  llvm::GlobalValue *gv = new llvm::GlobalVariable(getModule(), s->getType(),
    true, llvm::GlobalValue::PrivateLinkage, s, ".str");
  gv->setSection(AnnotationSection);
  gv->setUnnamedAddr(true);
  AnnotationStrings[Str] = gv;
  return gv;
}

llvm::Constant *CodeGenModule::EmitAnnotationUnit(SourceLocation Loc) {
  SourceManager &SM = getContext().getSourceManager();
  PresumedLoc PLoc = SM.getPresumedLoc(Loc);
  if (PLoc.isValid())
    return EmitAnnotationString(PLoc.getFilename());
  return EmitAnnotationString(SM.getBufferName(Loc));
}

llvm::Constant *CodeGenModule::EmitAnnotationLineNo(SourceLocation L) {
  SourceManager &SM = getContext().getSourceManager();
  PresumedLoc PLoc = SM.getPresumedLoc(L);
  unsigned LineNo = PLoc.isValid() ? PLoc.getLine() :
    SM.getExpansionLineNumber(L);
  return llvm::ConstantInt::get(Int32Ty, LineNo);
}

llvm::Constant *CodeGenModule::EmitAnnotateAttr(llvm::GlobalValue *GV,
                                                const AnnotateAttr *AA,
                                                SourceLocation L) {
  // Get the globals for file name, annotation, and the line number.
  llvm::Constant *AnnoGV = EmitAnnotationString(AA->getAnnotation()),
                 *UnitGV = EmitAnnotationUnit(L),
                 *LineNoCst = EmitAnnotationLineNo(L);

  // Create the ConstantStruct for the global annotation.
  llvm::Constant *Fields[4] = {
    llvm::ConstantExpr::getBitCast(GV, Int8PtrTy),
    llvm::ConstantExpr::getBitCast(AnnoGV, Int8PtrTy),
    llvm::ConstantExpr::getBitCast(UnitGV, Int8PtrTy),
    LineNoCst
  };
  return llvm::ConstantStruct::getAnon(Fields);
}

void CodeGenModule::AddGlobalAnnotations(const ValueDecl *D,
                                         llvm::GlobalValue *GV) {
  assert(D->hasAttr<AnnotateAttr>() && "no annotate attribute");
  // Get the struct elements for these annotations.
  for (specific_attr_iterator<AnnotateAttr>
       ai = D->specific_attr_begin<AnnotateAttr>(),
       ae = D->specific_attr_end<AnnotateAttr>(); ai != ae; ++ai)
    Annotations.push_back(EmitAnnotateAttr(GV, *ai, D->getLocation()));
}

bool CodeGenModule::MayDeferGeneration(const ValueDecl *Global) {
  // Never defer when EmitAllDecls is specified.
  if (LangOpts.EmitAllDecls)
    return false;

  return !getContext().DeclMustBeEmitted(Global);
}

llvm::Constant *CodeGenModule::GetWeakRefReference(const ValueDecl *VD) {
  const AliasAttr *AA = VD->getAttr<AliasAttr>();
  assert(AA && "No alias?");

  llvm::Type *DeclTy = getTypes().ConvertTypeForMem(VD->getType());

  // See if there is already something with the target's name in the module.
  llvm::GlobalValue *Entry = GetGlobalValue(AA->getAliasee());

  llvm::Constant *Aliasee;
  if (isa<llvm::FunctionType>(DeclTy))
    Aliasee = GetOrCreateLLVMFunction(AA->getAliasee(), DeclTy, GlobalDecl(),
                                      /*ForVTable=*/false);
  else
    Aliasee = GetOrCreateLLVMGlobal(AA->getAliasee(),
                                    llvm::PointerType::getUnqual(DeclTy), 0);
  if (!Entry) {
    llvm::GlobalValue* F = cast<llvm::GlobalValue>(Aliasee);
    F->setLinkage(llvm::Function::ExternalWeakLinkage);    
    WeakRefReferences.insert(F);
  }

  return Aliasee;
}

void CodeGenModule::EmitGlobal(GlobalDecl GD) {
  const ValueDecl *Global = cast<ValueDecl>(GD.getDecl());

  // Weak references don't produce any output by themselves.
  if (Global->hasAttr<WeakRefAttr>())
    return;

  // If this is an alias definition (which otherwise looks like a declaration)
  // emit it now.
  if (Global->hasAttr<AliasAttr>())
    return EmitAliasDefinition(GD);

  // If this is CUDA, be selective about which declarations we emit.
  if (LangOpts.CUDA) {
    if (CodeGenOpts.CUDAIsDevice) {
      if (!Global->hasAttr<CUDADeviceAttr>() &&
          !Global->hasAttr<CUDAGlobalAttr>() &&
          !Global->hasAttr<CUDAConstantAttr>() &&
          !Global->hasAttr<CUDASharedAttr>())
        return;
    } else {
      if (!Global->hasAttr<CUDAHostAttr>() && (
            Global->hasAttr<CUDADeviceAttr>() ||
            Global->hasAttr<CUDAConstantAttr>() ||
            Global->hasAttr<CUDASharedAttr>()))
        return;
    }
  }

  // Ignore declarations, they will be emitted on their first use.
  if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(Global)) {
    // Forward declarations are emitted lazily on first use.
    if (!FD->doesThisDeclarationHaveABody()) {
      if (!FD->doesDeclarationForceExternallyVisibleDefinition())
        return;

      const FunctionDecl *InlineDefinition = 0;
      FD->getBody(InlineDefinition);

      StringRef MangledName = getMangledName(GD);
      DeferredDecls.erase(MangledName);
      EmitGlobalDefinition(InlineDefinition);
      return;
    }
  } else {
    const VarDecl *VD = cast<VarDecl>(Global);
    assert(VD->isFileVarDecl() && "Cannot emit local var decl as global.");

    if (VD->isThisDeclarationADefinition() != VarDecl::Definition)
      return;
  }

  // Defer code generation when possible if this is a static definition, inline
  // function etc.  These we only want to emit if they are used.
  if (!MayDeferGeneration(Global)) {
    // Emit the definition if it can't be deferred.
    EmitGlobalDefinition(GD);
    return;
  }

  // If we're deferring emission of a C++ variable with an
  // initializer, remember the order in which it appeared in the file.
  if (getLangOpts().CPlusPlus && isa<VarDecl>(Global) &&
      cast<VarDecl>(Global)->hasInit()) {
    DelayedCXXInitPosition[Global] = CXXGlobalInits.size();
    CXXGlobalInits.push_back(0);
  }
  
  // If the value has already been used, add it directly to the
  // DeferredDeclsToEmit list.
  StringRef MangledName = getMangledName(GD);
  if (GetGlobalValue(MangledName))
    DeferredDeclsToEmit.push_back(GD);
  else {
    // Otherwise, remember that we saw a deferred decl with this name.  The
    // first use of the mangled name will cause it to move into
    // DeferredDeclsToEmit.
    DeferredDecls[MangledName] = GD;
  }
}

namespace {
  struct FunctionIsDirectlyRecursive :
    public RecursiveASTVisitor<FunctionIsDirectlyRecursive> {
    const StringRef Name;
    const Builtin::Context &BI;
    bool Result;
    FunctionIsDirectlyRecursive(StringRef N, const Builtin::Context &C) :
      Name(N), BI(C), Result(false) {
    }
    typedef RecursiveASTVisitor<FunctionIsDirectlyRecursive> Base;

    bool TraverseCallExpr(CallExpr *E) {
      const FunctionDecl *FD = E->getDirectCallee();
      if (!FD)
        return true;
      AsmLabelAttr *Attr = FD->getAttr<AsmLabelAttr>();
      if (Attr && Name == Attr->getLabel()) {
        Result = true;
        return false;
      }
      unsigned BuiltinID = FD->getBuiltinID();
      if (!BuiltinID)
        return true;
      StringRef BuiltinName = BI.GetName(BuiltinID);
      if (BuiltinName.startswith("__builtin_") &&
          Name == BuiltinName.slice(strlen("__builtin_"), StringRef::npos)) {
        Result = true;
        return false;
      }
      return true;
    }
  };
}

// isTriviallyRecursive - Check if this function calls another
// decl that, because of the asm attribute or the other decl being a builtin,
// ends up pointing to itself.
bool
CodeGenModule::isTriviallyRecursive(const FunctionDecl *FD) {
  StringRef Name;
  if (getCXXABI().getMangleContext().shouldMangleDeclName(FD)) {
    // asm labels are a special kind of mangling we have to support.
    AsmLabelAttr *Attr = FD->getAttr<AsmLabelAttr>();
    if (!Attr)
      return false;
    Name = Attr->getLabel();
  } else {
    Name = FD->getName();
  }

  FunctionIsDirectlyRecursive Walker(Name, Context.BuiltinInfo);
  Walker.TraverseFunctionDecl(const_cast<FunctionDecl*>(FD));
  return Walker.Result;
}

bool
CodeGenModule::shouldEmitFunction(const FunctionDecl *F) {
  if (getFunctionLinkage(F) != llvm::Function::AvailableExternallyLinkage)
    return true;
  if (CodeGenOpts.OptimizationLevel == 0 &&
      !F->hasAttr<AlwaysInlineAttr>())
    return false;
  // PR9614. Avoid cases where the source code is lying to us. An available
  // externally function should have an equivalent function somewhere else,
  // but a function that calls itself is clearly not equivalent to the real
  // implementation.
  // This happens in glibc's btowc and in some configure checks.
  return !isTriviallyRecursive(F);
}

void CodeGenModule::EmitGlobalDefinition(GlobalDecl GD) {
  const ValueDecl *D = cast<ValueDecl>(GD.getDecl());

  PrettyStackTraceDecl CrashInfo(const_cast<ValueDecl *>(D), D->getLocation(), 
                                 Context.getSourceManager(),
                                 "Generating code for declaration");
  
  if (const FunctionDecl *Function = dyn_cast<FunctionDecl>(D)) {
    // At -O0, don't generate IR for functions with available_externally 
    // linkage.
    if (!shouldEmitFunction(Function))
      return;

    if (const CXXMethodDecl *Method = dyn_cast<CXXMethodDecl>(D)) {
      // Make sure to emit the definition(s) before we emit the thunks.
      // This is necessary for the generation of certain thunks.
      if (const CXXConstructorDecl *CD = dyn_cast<CXXConstructorDecl>(Method))
        EmitCXXConstructor(CD, GD.getCtorType());
      else if (const CXXDestructorDecl *DD =dyn_cast<CXXDestructorDecl>(Method))
        EmitCXXDestructor(DD, GD.getDtorType());
      else
        EmitGlobalFunctionDefinition(GD);

      if (Method->isVirtual())
        getVTables().EmitThunks(GD);

      return;
    }

    return EmitGlobalFunctionDefinition(GD);
  }
  
  if (const VarDecl *VD = dyn_cast<VarDecl>(D))
    return EmitGlobalVarDefinition(VD);
  
  llvm_unreachable("Invalid argument to EmitGlobalDefinition()");
}

/// GetOrCreateLLVMFunction - If the specified mangled name is not in the
/// module, create and return an llvm Function with the specified type. If there
/// is something in the module with the specified name, return it potentially
/// bitcasted to the right type.
///
/// If D is non-null, it specifies a decl that correspond to this.  This is used
/// to set the attributes on the function when it is first created.
llvm::Constant *
CodeGenModule::GetOrCreateLLVMFunction(StringRef MangledName,
                                       llvm::Type *Ty,
                                       GlobalDecl D, bool ForVTable,
                                       llvm::Attributes ExtraAttrs) {
  // Lookup the entry, lazily creating it if necessary.
  llvm::GlobalValue *Entry = GetGlobalValue(MangledName);
  if (Entry) {
    if (WeakRefReferences.count(Entry)) {
      const FunctionDecl *FD = cast_or_null<FunctionDecl>(D.getDecl());
      if (FD && !FD->hasAttr<WeakAttr>())
        Entry->setLinkage(llvm::Function::ExternalLinkage);

      WeakRefReferences.erase(Entry);
    }

    if (Entry->getType()->getElementType() == Ty)
      return Entry;

    // Make sure the result is of the correct type.
    return llvm::ConstantExpr::getBitCast(Entry, Ty->getPointerTo());
  }

  // This function doesn't have a complete type (for example, the return
  // type is an incomplete struct). Use a fake type instead, and make
  // sure not to try to set attributes.
  bool IsIncompleteFunction = false;

  llvm::FunctionType *FTy;
  if (isa<llvm::FunctionType>(Ty)) {
    FTy = cast<llvm::FunctionType>(Ty);
  } else {
    FTy = llvm::FunctionType::get(VoidTy, false);
    IsIncompleteFunction = true;
  }
  
  llvm::Function *F = llvm::Function::Create(FTy,
                                             llvm::Function::ExternalLinkage,
                                             MangledName, &getModule());
  assert(F->getName() == MangledName && "name was uniqued!");
  if (D.getDecl())
    SetFunctionAttributes(D, F, IsIncompleteFunction);
  if (ExtraAttrs != llvm::Attribute::None)
    F->addFnAttr(ExtraAttrs);

  // This is the first use or definition of a mangled name.  If there is a
  // deferred decl with this name, remember that we need to emit it at the end
  // of the file.
  llvm::StringMap<GlobalDecl>::iterator DDI = DeferredDecls.find(MangledName);
  if (DDI != DeferredDecls.end()) {
    // Move the potentially referenced deferred decl to the DeferredDeclsToEmit
    // list, and remove it from DeferredDecls (since we don't need it anymore).
    DeferredDeclsToEmit.push_back(DDI->second);
    DeferredDecls.erase(DDI);

  // Otherwise, there are cases we have to worry about where we're
  // using a declaration for which we must emit a definition but where
  // we might not find a top-level definition:
  //   - member functions defined inline in their classes
  //   - friend functions defined inline in some class
  //   - special member functions with implicit definitions
  // If we ever change our AST traversal to walk into class methods,
  // this will be unnecessary.
  //
  // We also don't emit a definition for a function if it's going to be an entry
  // in a vtable, unless it's already marked as used.
  } else if (getLangOpts().CPlusPlus && D.getDecl()) {
    // Look for a declaration that's lexically in a record.
    const FunctionDecl *FD = cast<FunctionDecl>(D.getDecl());
    do {
      if (isa<CXXRecordDecl>(FD->getLexicalDeclContext())) {
        if (FD->isImplicit() && !ForVTable) {
          assert(FD->isUsed() && "Sema didn't mark implicit function as used!");
          DeferredDeclsToEmit.push_back(D.getWithDecl(FD));
          break;
        } else if (FD->doesThisDeclarationHaveABody()) {
          DeferredDeclsToEmit.push_back(D.getWithDecl(FD));
          break;
        }
      }
      FD = FD->getPreviousDecl();
    } while (FD);
  }

  // Make sure the result is of the requested type.
  if (!IsIncompleteFunction) {
    assert(F->getType()->getElementType() == Ty);
    return F;
  }

  llvm::Type *PTy = llvm::PointerType::getUnqual(Ty);
  return llvm::ConstantExpr::getBitCast(F, PTy);
}

/// GetAddrOfFunction - Return the address of the given function.  If Ty is
/// non-null, then this function will use the specified type if it has to
/// create it (this occurs when we see a definition of the function).
llvm::Constant *CodeGenModule::GetAddrOfFunction(GlobalDecl GD,
                                                 llvm::Type *Ty,
                                                 bool ForVTable) {
  // If there was no specific requested type, just convert it now.
  if (!Ty)
    Ty = getTypes().ConvertType(cast<ValueDecl>(GD.getDecl())->getType());
  
  StringRef MangledName = getMangledName(GD);
  return GetOrCreateLLVMFunction(MangledName, Ty, GD, ForVTable);
}

/// CreateRuntimeFunction - Create a new runtime function with the specified
/// type and name.
llvm::Constant *
CodeGenModule::CreateRuntimeFunction(llvm::FunctionType *FTy,
                                     StringRef Name,
                                     llvm::Attributes ExtraAttrs) {
  return GetOrCreateLLVMFunction(Name, FTy, GlobalDecl(), /*ForVTable=*/false,
                                 ExtraAttrs);
}

/// isTypeConstant - Determine whether an object of this type can be emitted
/// as a constant.
///
/// If ExcludeCtor is true, the duration when the object's constructor runs
/// will not be considered. The caller will need to verify that the object is
/// not written to during its construction.
bool CodeGenModule::isTypeConstant(QualType Ty, bool ExcludeCtor) {
  if (!Ty.isConstant(Context) && !Ty->isReferenceType())
    return false;

  if (Context.getLangOpts().CPlusPlus) {
    if (const CXXRecordDecl *Record
          = Context.getBaseElementType(Ty)->getAsCXXRecordDecl())
      return ExcludeCtor && !Record->hasMutableFields() &&
             Record->hasTrivialDestructor();
  }

  return true;
}

/// GetOrCreateLLVMGlobal - If the specified mangled name is not in the module,
/// create and return an llvm GlobalVariable with the specified type.  If there
/// is something in the module with the specified name, return it potentially
/// bitcasted to the right type.
///
/// If D is non-null, it specifies a decl that correspond to this.  This is used
/// to set the attributes on the global when it is first created.
llvm::Constant *
CodeGenModule::GetOrCreateLLVMGlobal(StringRef MangledName,
                                     llvm::PointerType *Ty,
                                     const VarDecl *D,
                                     bool UnnamedAddr) {
  // Lookup the entry, lazily creating it if necessary.
  llvm::GlobalValue *Entry = GetGlobalValue(MangledName);
  if (Entry) {
    if (WeakRefReferences.count(Entry)) {
      if (D && !D->hasAttr<WeakAttr>())
        Entry->setLinkage(llvm::Function::ExternalLinkage);

      WeakRefReferences.erase(Entry);
    }

    if (UnnamedAddr)
      Entry->setUnnamedAddr(true);

    if (Entry->getType() == Ty)
      return Entry;

    // Make sure the result is of the correct type.
    return llvm::ConstantExpr::getBitCast(Entry, Ty);
  }

  // This is the first use or definition of a mangled name.  If there is a
  // deferred decl with this name, remember that we need to emit it at the end
  // of the file.
  llvm::StringMap<GlobalDecl>::iterator DDI = DeferredDecls.find(MangledName);
  if (DDI != DeferredDecls.end()) {
    // Move the potentially referenced deferred decl to the DeferredDeclsToEmit
    // list, and remove it from DeferredDecls (since we don't need it anymore).
    DeferredDeclsToEmit.push_back(DDI->second);
    DeferredDecls.erase(DDI);
  }

  llvm::GlobalVariable *GV =
    new llvm::GlobalVariable(getModule(), Ty->getElementType(), false,
                             llvm::GlobalValue::ExternalLinkage,
                             0, MangledName, 0,
                             false, Ty->getAddressSpace());

  // Handle things which are present even on external declarations.
  if (D) {
    // FIXME: This code is overly simple and should be merged with other global
    // handling.
    GV->setConstant(isTypeConstant(D->getType(), false));

    // Set linkage and visibility in case we never see a definition.
    NamedDecl::LinkageInfo LV = D->getLinkageAndVisibility();
    if (LV.linkage() != ExternalLinkage) {
      // Don't set internal linkage on declarations.
    } else {
      if (D->hasAttr<DLLImportAttr>())
        GV->setLinkage(llvm::GlobalValue::DLLImportLinkage);
      else if (D->hasAttr<WeakAttr>() || D->isWeakImported())
        GV->setLinkage(llvm::GlobalValue::ExternalWeakLinkage);

      // Set visibility on a declaration only if it's explicit.
      if (LV.visibilityExplicit())
        GV->setVisibility(GetLLVMVisibility(LV.visibility()));
    }

    GV->setThreadLocal(D->isThreadSpecified());
  }

  return GV;
}


llvm::GlobalVariable *
CodeGenModule::CreateOrReplaceCXXRuntimeVariable(StringRef Name, 
                                      llvm::Type *Ty,
                                      llvm::GlobalValue::LinkageTypes Linkage) {
  llvm::GlobalVariable *GV = getModule().getNamedGlobal(Name);
  llvm::GlobalVariable *OldGV = 0;

  
  if (GV) {
    // Check if the variable has the right type.
    if (GV->getType()->getElementType() == Ty)
      return GV;

    // Because C++ name mangling, the only way we can end up with an already
    // existing global with the same name is if it has been declared extern "C".
      assert(GV->isDeclaration() && "Declaration has wrong type!");
    OldGV = GV;
  }
  
  // Create a new variable.
  GV = new llvm::GlobalVariable(getModule(), Ty, /*isConstant=*/true,
                                Linkage, 0, Name);
  
  if (OldGV) {
    // Replace occurrences of the old variable if needed.
    GV->takeName(OldGV);
    
    if (!OldGV->use_empty()) {
      llvm::Constant *NewPtrForOldDecl =
      llvm::ConstantExpr::getBitCast(GV, OldGV->getType());
      OldGV->replaceAllUsesWith(NewPtrForOldDecl);
    }
    
    OldGV->eraseFromParent();
  }
  
  return GV;
}

/// GetAddrOfGlobalVar - Return the llvm::Constant for the address of the
/// given global variable.  If Ty is non-null and if the global doesn't exist,
/// then it will be created with the specified type instead of whatever the
/// normal requested type would be.
llvm::Constant *CodeGenModule::GetAddrOfGlobalVar(const VarDecl *D,
                                                  llvm::Type *Ty) {
  assert(D->hasGlobalStorage() && "Not a global variable");
  QualType ASTTy = D->getType();
  if (Ty == 0)
    Ty = getTypes().ConvertTypeForMem(ASTTy);

  llvm::PointerType *PTy =
    llvm::PointerType::get(Ty, getContext().getTargetAddressSpace(ASTTy));

  StringRef MangledName = getMangledName(D);
  return GetOrCreateLLVMGlobal(MangledName, PTy, D);
}

/// CreateRuntimeVariable - Create a new runtime global variable with the
/// specified type and name.
llvm::Constant *
CodeGenModule::CreateRuntimeVariable(llvm::Type *Ty,
                                     StringRef Name) {
  return GetOrCreateLLVMGlobal(Name, llvm::PointerType::getUnqual(Ty), 0,
                               true);
}

void CodeGenModule::EmitTentativeDefinition(const VarDecl *D) {
  assert(!D->getInit() && "Cannot emit definite definitions here!");

  if (MayDeferGeneration(D)) {
    // If we have not seen a reference to this variable yet, place it
    // into the deferred declarations table to be emitted if needed
    // later.
    StringRef MangledName = getMangledName(D);
    if (!GetGlobalValue(MangledName)) {
      DeferredDecls[MangledName] = D;
      return;
    }
  }

  // The tentative definition is the only definition.
  EmitGlobalVarDefinition(D);
}

void CodeGenModule::EmitVTable(CXXRecordDecl *Class, bool DefinitionRequired) {
  if (DefinitionRequired)
    getVTables().GenerateClassData(getVTableLinkage(Class), Class);
}

llvm::GlobalVariable::LinkageTypes 
CodeGenModule::getVTableLinkage(const CXXRecordDecl *RD) {
  if (RD->getLinkage() != ExternalLinkage)
    return llvm::GlobalVariable::InternalLinkage;

  if (const CXXMethodDecl *KeyFunction
                                    = RD->getASTContext().getKeyFunction(RD)) {
    // If this class has a key function, use that to determine the linkage of
    // the vtable.
    const FunctionDecl *Def = 0;
    if (KeyFunction->hasBody(Def))
      KeyFunction = cast<CXXMethodDecl>(Def);
    
    switch (KeyFunction->getTemplateSpecializationKind()) {
      case TSK_Undeclared:
      case TSK_ExplicitSpecialization:
        // When compiling with optimizations turned on, we emit all vtables,
        // even if the key function is not defined in the current translation
        // unit. If this is the case, use available_externally linkage.
        if (!Def && CodeGenOpts.OptimizationLevel)
          return llvm::GlobalVariable::AvailableExternallyLinkage;

        if (KeyFunction->isInlined())
          return !Context.getLangOpts().AppleKext ?
                   llvm::GlobalVariable::LinkOnceODRLinkage :
                   llvm::Function::InternalLinkage;
        
        return llvm::GlobalVariable::ExternalLinkage;
        
      case TSK_ImplicitInstantiation:
        return !Context.getLangOpts().AppleKext ?
                 llvm::GlobalVariable::LinkOnceODRLinkage :
                 llvm::Function::InternalLinkage;

      case TSK_ExplicitInstantiationDefinition:
        return !Context.getLangOpts().AppleKext ?
                 llvm::GlobalVariable::WeakODRLinkage :
                 llvm::Function::InternalLinkage;
  
      case TSK_ExplicitInstantiationDeclaration:
        // FIXME: Use available_externally linkage. However, this currently
        // breaks LLVM's build due to undefined symbols.
        //      return llvm::GlobalVariable::AvailableExternallyLinkage;
        return !Context.getLangOpts().AppleKext ?
                 llvm::GlobalVariable::LinkOnceODRLinkage :
                 llvm::Function::InternalLinkage;
    }
  }
  
  if (Context.getLangOpts().AppleKext)
    return llvm::Function::InternalLinkage;
  
  switch (RD->getTemplateSpecializationKind()) {
  case TSK_Undeclared:
  case TSK_ExplicitSpecialization:
  case TSK_ImplicitInstantiation:
    // FIXME: Use available_externally linkage. However, this currently
    // breaks LLVM's build due to undefined symbols.
    //   return llvm::GlobalVariable::AvailableExternallyLinkage;
  case TSK_ExplicitInstantiationDeclaration:
    return llvm::GlobalVariable::LinkOnceODRLinkage;

  case TSK_ExplicitInstantiationDefinition:
      return llvm::GlobalVariable::WeakODRLinkage;
  }

  llvm_unreachable("Invalid TemplateSpecializationKind!");
}

CharUnits CodeGenModule::GetTargetTypeStoreSize(llvm::Type *Ty) const {
    return Context.toCharUnitsFromBits(
      TheTargetData.getTypeStoreSizeInBits(Ty));
}

llvm::Constant *
CodeGenModule::MaybeEmitGlobalStdInitializerListInitializer(const VarDecl *D,
                                                       const Expr *rawInit) {
  ArrayRef<ExprWithCleanups::CleanupObject> cleanups;
  if (const ExprWithCleanups *withCleanups =
          dyn_cast<ExprWithCleanups>(rawInit)) {
    cleanups = withCleanups->getObjects();
    rawInit = withCleanups->getSubExpr();
  }

  const InitListExpr *init = dyn_cast<InitListExpr>(rawInit);
  if (!init || !init->initializesStdInitializerList() ||
      init->getNumInits() == 0)
    return 0;

  ASTContext &ctx = getContext();
  unsigned numInits = init->getNumInits();
  // FIXME: This check is here because we would otherwise silently miscompile
  // nested global std::initializer_lists. Better would be to have a real
  // implementation.
  for (unsigned i = 0; i < numInits; ++i) {
    const InitListExpr *inner = dyn_cast<InitListExpr>(init->getInit(i));
    if (inner && inner->initializesStdInitializerList()) {
      ErrorUnsupported(inner, "nested global std::initializer_list");
      return 0;
    }
  }

  // Synthesize a fake VarDecl for the array and initialize that.
  QualType elementType = init->getInit(0)->getType();
  llvm::APInt numElements(ctx.getTypeSize(ctx.getSizeType()), numInits);
  QualType arrayType = ctx.getConstantArrayType(elementType, numElements,
                                                ArrayType::Normal, 0);

  IdentifierInfo *name = &ctx.Idents.get(D->getNameAsString() + "__initlist");
  TypeSourceInfo *sourceInfo = ctx.getTrivialTypeSourceInfo(
                                              arrayType, D->getLocation());
  VarDecl *backingArray = VarDecl::Create(ctx, const_cast<DeclContext*>(
                                                          D->getDeclContext()),
                                          D->getLocStart(), D->getLocation(),
                                          name, arrayType, sourceInfo,
                                          SC_Static, SC_Static);

  // Now clone the InitListExpr to initialize the array instead.
  // Incredible hack: we want to use the existing InitListExpr here, so we need
  // to tell it that it no longer initializes a std::initializer_list.
  Expr *arrayInit = new (ctx) InitListExpr(ctx, init->getLBraceLoc(),
                                    const_cast<InitListExpr*>(init)->getInits(),
                                                   init->getNumInits(),
                                                   init->getRBraceLoc());
  arrayInit->setType(arrayType);

  if (!cleanups.empty())
    arrayInit = ExprWithCleanups::Create(ctx, arrayInit, cleanups);

  backingArray->setInit(arrayInit);

  // Emit the definition of the array.
  EmitGlobalVarDefinition(backingArray);

  // Inspect the initializer list to validate it and determine its type.
  // FIXME: doing this every time is probably inefficient; caching would be nice
  RecordDecl *record = init->getType()->castAs<RecordType>()->getDecl();
  RecordDecl::field_iterator field = record->field_begin();
  if (field == record->field_end()) {
    ErrorUnsupported(D, "weird std::initializer_list");
    return 0;
  }
  QualType elementPtr = ctx.getPointerType(elementType.withConst());
  // Start pointer.
  if (!ctx.hasSameType(field->getType(), elementPtr)) {
    ErrorUnsupported(D, "weird std::initializer_list");
    return 0;
  }
  ++field;
  if (field == record->field_end()) {
    ErrorUnsupported(D, "weird std::initializer_list");
    return 0;
  }
  bool isStartEnd = false;
  if (ctx.hasSameType(field->getType(), elementPtr)) {
    // End pointer.
    isStartEnd = true;
  } else if(!ctx.hasSameType(field->getType(), ctx.getSizeType())) {
    ErrorUnsupported(D, "weird std::initializer_list");
    return 0;
  }

  // Now build an APValue representing the std::initializer_list.
  APValue initListValue(APValue::UninitStruct(), 0, 2);
  APValue &startField = initListValue.getStructField(0);
  APValue::LValuePathEntry startOffsetPathEntry;
  startOffsetPathEntry.ArrayIndex = 0;
  startField = APValue(APValue::LValueBase(backingArray),
                       CharUnits::fromQuantity(0),
                       llvm::makeArrayRef(startOffsetPathEntry),
                       /*IsOnePastTheEnd=*/false, 0);

  if (isStartEnd) {
    APValue &endField = initListValue.getStructField(1);
    APValue::LValuePathEntry endOffsetPathEntry;
    endOffsetPathEntry.ArrayIndex = numInits;
    endField = APValue(APValue::LValueBase(backingArray),
                       ctx.getTypeSizeInChars(elementType) * numInits,
                       llvm::makeArrayRef(endOffsetPathEntry),
                       /*IsOnePastTheEnd=*/true, 0);
  } else {
    APValue &sizeField = initListValue.getStructField(1);
    sizeField = APValue(llvm::APSInt(numElements));
  }

  // Emit the constant for the initializer_list.
  llvm::Constant *llvmInit =
      EmitConstantValueForMemory(initListValue, D->getType());
  assert(llvmInit && "failed to initialize as constant");
  return llvmInit;
}

void CodeGenModule::EmitGlobalVarDefinition(const VarDecl *D) {
  llvm::Constant *Init = 0;
  QualType ASTTy = D->getType();
  CXXRecordDecl *RD = ASTTy->getBaseElementTypeUnsafe()->getAsCXXRecordDecl();
  bool NeedsGlobalCtor = false;
  bool NeedsGlobalDtor = RD && !RD->hasTrivialDestructor();

  const VarDecl *InitDecl;
  const Expr *InitExpr = D->getAnyInitializer(InitDecl);

  if (!InitExpr) {
    // This is a tentative definition; tentative definitions are
    // implicitly initialized with { 0 }.
    //
    // Note that tentative definitions are only emitted at the end of
    // a translation unit, so they should never have incomplete
    // type. In addition, EmitTentativeDefinition makes sure that we
    // never attempt to emit a tentative definition if a real one
    // exists. A use may still exists, however, so we still may need
    // to do a RAUW.
    assert(!ASTTy->isIncompleteType() && "Unexpected incomplete type");
    Init = EmitNullConstant(D->getType());
  } else {
    // If this is a std::initializer_list, emit the special initializer.
    Init = MaybeEmitGlobalStdInitializerListInitializer(D, InitExpr);
    // An empty init list will perform zero-initialization, which happens
    // to be exactly what we want.
    // FIXME: It does so in a global constructor, which is *not* what we
    // want.

    if (!Init)
      Init = EmitConstantInit(*InitDecl);
    if (!Init) {
      QualType T = InitExpr->getType();
      if (D->getType()->isReferenceType())
        T = D->getType();

      if (getLangOpts().CPlusPlus) {
        Init = EmitNullConstant(T);
        NeedsGlobalCtor = true;
      } else {
        ErrorUnsupported(D, "static initializer");
        Init = llvm::UndefValue::get(getTypes().ConvertType(T));
      }
    } else {
      // We don't need an initializer, so remove the entry for the delayed
      // initializer position (just in case this entry was delayed) if we
      // also don't need to register a destructor.
      if (getLangOpts().CPlusPlus && !NeedsGlobalDtor)
        DelayedCXXInitPosition.erase(D);
    }
  }

  llvm::Type* InitType = Init->getType();
  llvm::Constant *Entry = GetAddrOfGlobalVar(D, InitType);

  // Strip off a bitcast if we got one back.
  if (llvm::ConstantExpr *CE = dyn_cast<llvm::ConstantExpr>(Entry)) {
    assert(CE->getOpcode() == llvm::Instruction::BitCast ||
           // all zero index gep.
           CE->getOpcode() == llvm::Instruction::GetElementPtr);
    Entry = CE->getOperand(0);
  }

  // Entry is now either a Function or GlobalVariable.
  llvm::GlobalVariable *GV = dyn_cast<llvm::GlobalVariable>(Entry);

  // We have a definition after a declaration with the wrong type.
  // We must make a new GlobalVariable* and update everything that used OldGV
  // (a declaration or tentative definition) with the new GlobalVariable*
  // (which will be a definition).
  //
  // This happens if there is a prototype for a global (e.g.
  // "extern int x[];") and then a definition of a different type (e.g.
  // "int x[10];"). This also happens when an initializer has a different type
  // from the type of the global (this happens with unions).
  if (GV == 0 ||
      GV->getType()->getElementType() != InitType ||
      GV->getType()->getAddressSpace() !=
        getContext().getTargetAddressSpace(ASTTy)) {

    // Move the old entry aside so that we'll create a new one.
    Entry->setName(StringRef());

    // Make a new global with the correct type, this is now guaranteed to work.
    GV = cast<llvm::GlobalVariable>(GetAddrOfGlobalVar(D, InitType));

    // Replace all uses of the old global with the new global
    llvm::Constant *NewPtrForOldDecl =
        llvm::ConstantExpr::getBitCast(GV, Entry->getType());
    Entry->replaceAllUsesWith(NewPtrForOldDecl);

    // Erase the old global, since it is no longer used.
    cast<llvm::GlobalValue>(Entry)->eraseFromParent();
  }

  if (D->hasAttr<AnnotateAttr>())
    AddGlobalAnnotations(D, GV);

  GV->setInitializer(Init);

  // If it is safe to mark the global 'constant', do so now.
  GV->setConstant(!NeedsGlobalCtor && !NeedsGlobalDtor &&
                  isTypeConstant(D->getType(), true));

  GV->setAlignment(getContext().getDeclAlign(D).getQuantity());

  // Set the llvm linkage type as appropriate.
  llvm::GlobalValue::LinkageTypes Linkage = 
    GetLLVMLinkageVarDefinition(D, GV);
  GV->setLinkage(Linkage);
  if (Linkage == llvm::GlobalVariable::CommonLinkage)
    // common vars aren't constant even if declared const.
    GV->setConstant(false);

  SetCommonAttributes(D, GV);

  // Emit the initializer function if necessary.
  if (NeedsGlobalCtor || NeedsGlobalDtor)
    EmitCXXGlobalVarDeclInitFunc(D, GV, NeedsGlobalCtor);

  // Emit global variable debug information.
  if (CGDebugInfo *DI = getModuleDebugInfo())
    if (getCodeGenOpts().DebugInfo >= CodeGenOptions::LimitedDebugInfo)
      DI->EmitGlobalVariable(GV, D);
}

llvm::GlobalValue::LinkageTypes
CodeGenModule::GetLLVMLinkageVarDefinition(const VarDecl *D,
                                           llvm::GlobalVariable *GV) {
  GVALinkage Linkage = getContext().GetGVALinkageForVariable(D);
  if (Linkage == GVA_Internal)
    return llvm::Function::InternalLinkage;
  else if (D->hasAttr<DLLImportAttr>())
    return llvm::Function::DLLImportLinkage;
  else if (D->hasAttr<DLLExportAttr>())
    return llvm::Function::DLLExportLinkage;
  else if (D->hasAttr<WeakAttr>()) {
    if (GV->isConstant())
      return llvm::GlobalVariable::WeakODRLinkage;
    else
      return llvm::GlobalVariable::WeakAnyLinkage;
  } else if (Linkage == GVA_TemplateInstantiation ||
             Linkage == GVA_ExplicitTemplateInstantiation)
    return llvm::GlobalVariable::WeakODRLinkage;
  else if (!getLangOpts().CPlusPlus && 
           ((!CodeGenOpts.NoCommon && !D->getAttr<NoCommonAttr>()) ||
             D->getAttr<CommonAttr>()) &&
           !D->hasExternalStorage() && !D->getInit() &&
           !D->getAttr<SectionAttr>() && !D->isThreadSpecified() &&
           !D->getAttr<WeakImportAttr>()) {
    // Thread local vars aren't considered common linkage.
    return llvm::GlobalVariable::CommonLinkage;
  }
  return llvm::GlobalVariable::ExternalLinkage;
}

/// ReplaceUsesOfNonProtoTypeWithRealFunction - This function is called when we
/// implement a function with no prototype, e.g. "int foo() {}".  If there are
/// existing call uses of the old function in the module, this adjusts them to
/// call the new function directly.
///
/// This is not just a cleanup: the always_inline pass requires direct calls to
/// functions to be able to inline them.  If there is a bitcast in the way, it
/// won't inline them.  Instcombine normally deletes these calls, but it isn't
/// run at -O0.
static void ReplaceUsesOfNonProtoTypeWithRealFunction(llvm::GlobalValue *Old,
                                                      llvm::Function *NewFn) {
  // If we're redefining a global as a function, don't transform it.
  llvm::Function *OldFn = dyn_cast<llvm::Function>(Old);
  if (OldFn == 0) return;

  llvm::Type *NewRetTy = NewFn->getReturnType();
  SmallVector<llvm::Value*, 4> ArgList;

  for (llvm::Value::use_iterator UI = OldFn->use_begin(), E = OldFn->use_end();
       UI != E; ) {
    // TODO: Do invokes ever occur in C code?  If so, we should handle them too.
    llvm::Value::use_iterator I = UI++; // Increment before the CI is erased.
    llvm::CallInst *CI = dyn_cast<llvm::CallInst>(*I);
    if (!CI) continue; // FIXME: when we allow Invoke, just do CallSite CS(*I)
    llvm::CallSite CS(CI);
    if (!CI || !CS.isCallee(I)) continue;

    // If the return types don't match exactly, and if the call isn't dead, then
    // we can't transform this call.
    if (CI->getType() != NewRetTy && !CI->use_empty())
      continue;

    // Get the attribute list.
    llvm::SmallVector<llvm::AttributeWithIndex, 8> AttrVec;
    llvm::AttrListPtr AttrList = CI->getAttributes();

    // Get any return attributes.
    llvm::Attributes RAttrs = AttrList.getRetAttributes();

    // Add the return attributes.
    if (RAttrs)
      AttrVec.push_back(llvm::AttributeWithIndex::get(0, RAttrs));

    // If the function was passed too few arguments, don't transform.  If extra
    // arguments were passed, we silently drop them.  If any of the types
    // mismatch, we don't transform.
    unsigned ArgNo = 0;
    bool DontTransform = false;
    for (llvm::Function::arg_iterator AI = NewFn->arg_begin(),
         E = NewFn->arg_end(); AI != E; ++AI, ++ArgNo) {
      if (CS.arg_size() == ArgNo ||
          CS.getArgument(ArgNo)->getType() != AI->getType()) {
        DontTransform = true;
        break;
      }

      // Add any parameter attributes.
      if (llvm::Attributes PAttrs = AttrList.getParamAttributes(ArgNo + 1))
        AttrVec.push_back(llvm::AttributeWithIndex::get(ArgNo + 1, PAttrs));
    }
    if (DontTransform)
      continue;

    if (llvm::Attributes FnAttrs =  AttrList.getFnAttributes())
      AttrVec.push_back(llvm::AttributeWithIndex::get(~0, FnAttrs));

    // Okay, we can transform this.  Create the new call instruction and copy
    // over the required information.
    ArgList.append(CS.arg_begin(), CS.arg_begin() + ArgNo);
    llvm::CallInst *NewCall = llvm::CallInst::Create(NewFn, ArgList, "", CI);
    ArgList.clear();
    if (!NewCall->getType()->isVoidTy())
      NewCall->takeName(CI);
    NewCall->setAttributes(llvm::AttrListPtr::get(AttrVec.begin(),
                                                  AttrVec.end()));
    NewCall->setCallingConv(CI->getCallingConv());

    // Finally, remove the old call, replacing any uses with the new one.
    if (!CI->use_empty())
      CI->replaceAllUsesWith(NewCall);

    // Copy debug location attached to CI.
    if (!CI->getDebugLoc().isUnknown())
      NewCall->setDebugLoc(CI->getDebugLoc());
    CI->eraseFromParent();
  }
}

void CodeGenModule::HandleCXXStaticMemberVarInstantiation(VarDecl *VD) {
  TemplateSpecializationKind TSK = VD->getTemplateSpecializationKind();
  // If we have a definition, this might be a deferred decl. If the
  // instantiation is explicit, make sure we emit it at the end.
  if (VD->getDefinition() && TSK == TSK_ExplicitInstantiationDefinition)
    GetAddrOfGlobalVar(VD);
}

void CodeGenModule::EmitGlobalFunctionDefinition(GlobalDecl GD) {
  const FunctionDecl *D = cast<FunctionDecl>(GD.getDecl());

  // Compute the function info and LLVM type.
  const CGFunctionInfo &FI = getTypes().arrangeGlobalDeclaration(GD);
  llvm::FunctionType *Ty = getTypes().GetFunctionType(FI);

  // Get or create the prototype for the function.
  llvm::Constant *Entry = GetAddrOfFunction(GD, Ty);

  // Strip off a bitcast if we got one back.
  if (llvm::ConstantExpr *CE = dyn_cast<llvm::ConstantExpr>(Entry)) {
    assert(CE->getOpcode() == llvm::Instruction::BitCast);
    Entry = CE->getOperand(0);
  }


  if (cast<llvm::GlobalValue>(Entry)->getType()->getElementType() != Ty) {
    llvm::GlobalValue *OldFn = cast<llvm::GlobalValue>(Entry);

    // If the types mismatch then we have to rewrite the definition.
    assert(OldFn->isDeclaration() &&
           "Shouldn't replace non-declaration");

    // F is the Function* for the one with the wrong type, we must make a new
    // Function* and update everything that used F (a declaration) with the new
    // Function* (which will be a definition).
    //
    // This happens if there is a prototype for a function
    // (e.g. "int f()") and then a definition of a different type
    // (e.g. "int f(int x)").  Move the old function aside so that it
    // doesn't interfere with GetAddrOfFunction.
    OldFn->setName(StringRef());
    llvm::Function *NewFn = cast<llvm::Function>(GetAddrOfFunction(GD, Ty));

    // If this is an implementation of a function without a prototype, try to
    // replace any existing uses of the function (which may be calls) with uses
    // of the new function
    if (D->getType()->isFunctionNoProtoType()) {
      ReplaceUsesOfNonProtoTypeWithRealFunction(OldFn, NewFn);
      OldFn->removeDeadConstantUsers();
    }

    // Replace uses of F with the Function we will endow with a body.
    if (!Entry->use_empty()) {
      llvm::Constant *NewPtrForOldDecl =
        llvm::ConstantExpr::getBitCast(NewFn, Entry->getType());
      Entry->replaceAllUsesWith(NewPtrForOldDecl);
    }

    // Ok, delete the old function now, which is dead.
    OldFn->eraseFromParent();

    Entry = NewFn;
  }

  // We need to set linkage and visibility on the function before
  // generating code for it because various parts of IR generation
  // want to propagate this information down (e.g. to local static
  // declarations).
  llvm::Function *Fn = cast<llvm::Function>(Entry);
  setFunctionLinkage(D, Fn);

  // FIXME: this is redundant with part of SetFunctionDefinitionAttributes
  setGlobalVisibility(Fn, D);

  CodeGenFunction(*this).GenerateCode(D, Fn, FI);

  SetFunctionDefinitionAttributes(D, Fn);
  SetLLVMFunctionAttributesForDefinition(D, Fn);

  if (const ConstructorAttr *CA = D->getAttr<ConstructorAttr>())
    AddGlobalCtor(Fn, CA->getPriority());
  if (const DestructorAttr *DA = D->getAttr<DestructorAttr>())
    AddGlobalDtor(Fn, DA->getPriority());
  if (D->hasAttr<AnnotateAttr>())
    AddGlobalAnnotations(D, Fn);
}

void CodeGenModule::EmitAliasDefinition(GlobalDecl GD) {
  const ValueDecl *D = cast<ValueDecl>(GD.getDecl());
  const AliasAttr *AA = D->getAttr<AliasAttr>();
  assert(AA && "Not an alias?");

  StringRef MangledName = getMangledName(GD);

  // If there is a definition in the module, then it wins over the alias.
  // This is dubious, but allow it to be safe.  Just ignore the alias.
  llvm::GlobalValue *Entry = GetGlobalValue(MangledName);
  if (Entry && !Entry->isDeclaration())
    return;

  llvm::Type *DeclTy = getTypes().ConvertTypeForMem(D->getType());

  // Create a reference to the named value.  This ensures that it is emitted
  // if a deferred decl.
  llvm::Constant *Aliasee;
  if (isa<llvm::FunctionType>(DeclTy))
    Aliasee = GetOrCreateLLVMFunction(AA->getAliasee(), DeclTy, GlobalDecl(),
                                      /*ForVTable=*/false);
  else
    Aliasee = GetOrCreateLLVMGlobal(AA->getAliasee(),
                                    llvm::PointerType::getUnqual(DeclTy), 0);

  // Create the new alias itself, but don't set a name yet.
  llvm::GlobalValue *GA =
    new llvm::GlobalAlias(Aliasee->getType(),
                          llvm::Function::ExternalLinkage,
                          "", Aliasee, &getModule());

  if (Entry) {
    assert(Entry->isDeclaration());

    // If there is a declaration in the module, then we had an extern followed
    // by the alias, as in:
    //   extern int test6();
    //   ...
    //   int test6() __attribute__((alias("test7")));
    //
    // Remove it and replace uses of it with the alias.
    GA->takeName(Entry);

    Entry->replaceAllUsesWith(llvm::ConstantExpr::getBitCast(GA,
                                                          Entry->getType()));
    Entry->eraseFromParent();
  } else {
    GA->setName(MangledName);
  }

  // Set attributes which are particular to an alias; this is a
  // specialization of the attributes which may be set on a global
  // variable/function.
  if (D->hasAttr<DLLExportAttr>()) {
    if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(D)) {
      // The dllexport attribute is ignored for undefined symbols.
      if (FD->hasBody())
        GA->setLinkage(llvm::Function::DLLExportLinkage);
    } else {
      GA->setLinkage(llvm::Function::DLLExportLinkage);
    }
  } else if (D->hasAttr<WeakAttr>() ||
             D->hasAttr<WeakRefAttr>() ||
             D->isWeakImported()) {
    GA->setLinkage(llvm::Function::WeakAnyLinkage);
  }

  SetCommonAttributes(D, GA);
}

llvm::Function *CodeGenModule::getIntrinsic(unsigned IID,
                                            ArrayRef<llvm::Type*> Tys) {
  return llvm::Intrinsic::getDeclaration(&getModule(), (llvm::Intrinsic::ID)IID,
                                         Tys);
}

static llvm::StringMapEntry<llvm::Constant*> &
GetConstantCFStringEntry(llvm::StringMap<llvm::Constant*> &Map,
                         const StringLiteral *Literal,
                         bool TargetIsLSB,
                         bool &IsUTF16,
                         unsigned &StringLength) {
  StringRef String = Literal->getString();
  unsigned NumBytes = String.size();

  // Check for simple case.
  if (!Literal->containsNonAsciiOrNull()) {
    StringLength = NumBytes;
    return Map.GetOrCreateValue(String);
  }

  // Otherwise, convert the UTF8 literals into a string of shorts.
  IsUTF16 = true;

  SmallVector<UTF16, 128> ToBuf(NumBytes + 1); // +1 for ending nulls.
  const UTF8 *FromPtr = (UTF8 *)String.data();
  UTF16 *ToPtr = &ToBuf[0];

  (void)ConvertUTF8toUTF16(&FromPtr, FromPtr + NumBytes,
                           &ToPtr, ToPtr + NumBytes,
                           strictConversion);

  // ConvertUTF8toUTF16 returns the length in ToPtr.
  StringLength = ToPtr - &ToBuf[0];

  // Add an explicit null.
  *ToPtr = 0;
  return Map.
    GetOrCreateValue(StringRef(reinterpret_cast<const char *>(ToBuf.data()),
                               (StringLength + 1) * 2));
}

static llvm::StringMapEntry<llvm::Constant*> &
GetConstantStringEntry(llvm::StringMap<llvm::Constant*> &Map,
                       const StringLiteral *Literal,
                       unsigned &StringLength) {
  StringRef String = Literal->getString();
  StringLength = String.size();
  return Map.GetOrCreateValue(String);
}

llvm::Constant *
CodeGenModule::GetAddrOfConstantCFString(const StringLiteral *Literal) {
  unsigned StringLength = 0;
  bool isUTF16 = false;
  llvm::StringMapEntry<llvm::Constant*> &Entry =
    GetConstantCFStringEntry(CFConstantStringMap, Literal,
                             getTargetData().isLittleEndian(),
                             isUTF16, StringLength);

  if (llvm::Constant *C = Entry.getValue())
    return C;

  llvm::Constant *Zero = llvm::Constant::getNullValue(Int32Ty);
  llvm::Constant *Zeros[] = { Zero, Zero };

  // If we don't already have it, get __CFConstantStringClassReference.
  if (!CFConstantStringClassRef) {
    llvm::Type *Ty = getTypes().ConvertType(getContext().IntTy);
    Ty = llvm::ArrayType::get(Ty, 0);
    llvm::Constant *GV = CreateRuntimeVariable(Ty,
                                           "__CFConstantStringClassReference");
    // Decay array -> ptr
    CFConstantStringClassRef =
      llvm::ConstantExpr::getGetElementPtr(GV, Zeros);
  }

  QualType CFTy = getContext().getCFConstantStringType();

  llvm::StructType *STy =
    cast<llvm::StructType>(getTypes().ConvertType(CFTy));

  llvm::Constant *Fields[4];

  // Class pointer.
  Fields[0] = CFConstantStringClassRef;

  // Flags.
  llvm::Type *Ty = getTypes().ConvertType(getContext().UnsignedIntTy);
  Fields[1] = isUTF16 ? llvm::ConstantInt::get(Ty, 0x07d0) :
    llvm::ConstantInt::get(Ty, 0x07C8);

  // String pointer.
  llvm::Constant *C = 0;
  if (isUTF16) {
    ArrayRef<uint16_t> Arr =
      llvm::makeArrayRef<uint16_t>((uint16_t*)Entry.getKey().data(),
                                   Entry.getKey().size() / 2);
    C = llvm::ConstantDataArray::get(VMContext, Arr);
  } else {
    C = llvm::ConstantDataArray::getString(VMContext, Entry.getKey());
  }

  llvm::GlobalValue::LinkageTypes Linkage;
  if (isUTF16)
    // FIXME: why do utf strings get "_" labels instead of "L" labels?
    Linkage = llvm::GlobalValue::InternalLinkage;
  else
    // FIXME: With OS X ld 123.2 (xcode 4) and LTO we would get a linker error
    // when using private linkage. It is not clear if this is a bug in ld
    // or a reasonable new restriction.
    Linkage = llvm::GlobalValue::LinkerPrivateLinkage;
  
  // Note: -fwritable-strings doesn't make the backing store strings of
  // CFStrings writable. (See <rdar://problem/10657500>)
  llvm::GlobalVariable *GV =
    new llvm::GlobalVariable(getModule(), C->getType(), /*isConstant=*/true,
                             Linkage, C, ".str");
  GV->setUnnamedAddr(true);
  if (isUTF16) {
    CharUnits Align = getContext().getTypeAlignInChars(getContext().ShortTy);
    GV->setAlignment(Align.getQuantity());
  } else {
    CharUnits Align = getContext().getTypeAlignInChars(getContext().CharTy);
    GV->setAlignment(Align.getQuantity());
  }

  // String.
  Fields[2] = llvm::ConstantExpr::getGetElementPtr(GV, Zeros);

  if (isUTF16)
    // Cast the UTF16 string to the correct type.
    Fields[2] = llvm::ConstantExpr::getBitCast(Fields[2], Int8PtrTy);

  // String length.
  Ty = getTypes().ConvertType(getContext().LongTy);
  Fields[3] = llvm::ConstantInt::get(Ty, StringLength);

  // The struct.
  C = llvm::ConstantStruct::get(STy, Fields);
  GV = new llvm::GlobalVariable(getModule(), C->getType(), true,
                                llvm::GlobalVariable::PrivateLinkage, C,
                                "_unnamed_cfstring_");
  if (const char *Sect = getContext().getTargetInfo().getCFStringSection())
    GV->setSection(Sect);
  Entry.setValue(GV);

  return GV;
}

static RecordDecl *
CreateRecordDecl(const ASTContext &Ctx, RecordDecl::TagKind TK,
                 DeclContext *DC, IdentifierInfo *Id) {
  SourceLocation Loc;
  if (Ctx.getLangOpts().CPlusPlus)
    return CXXRecordDecl::Create(Ctx, TK, DC, Loc, Loc, Id);
  else
    return RecordDecl::Create(Ctx, TK, DC, Loc, Loc, Id);
}

llvm::Constant *
CodeGenModule::GetAddrOfConstantString(const StringLiteral *Literal) {
  unsigned StringLength = 0;
  llvm::StringMapEntry<llvm::Constant*> &Entry =
    GetConstantStringEntry(CFConstantStringMap, Literal, StringLength);
  
  if (llvm::Constant *C = Entry.getValue())
    return C;
  
  llvm::Constant *Zero = llvm::Constant::getNullValue(Int32Ty);
  llvm::Constant *Zeros[] = { Zero, Zero };
  
  // If we don't already have it, get _NSConstantStringClassReference.
  if (!ConstantStringClassRef) {
    std::string StringClass(getLangOpts().ObjCConstantStringClass);
    llvm::Type *Ty = getTypes().ConvertType(getContext().IntTy);
    llvm::Constant *GV;
    if (LangOpts.ObjCNonFragileABI) {
      std::string str = 
        StringClass.empty() ? "OBJC_CLASS_$_NSConstantString" 
                            : "OBJC_CLASS_$_" + StringClass;
      GV = getObjCRuntime().GetClassGlobal(str);
      // Make sure the result is of the correct type.
      llvm::Type *PTy = llvm::PointerType::getUnqual(Ty);
      ConstantStringClassRef =
        llvm::ConstantExpr::getBitCast(GV, PTy);
    } else {
      std::string str =
        StringClass.empty() ? "_NSConstantStringClassReference"
                            : "_" + StringClass + "ClassReference";
      llvm::Type *PTy = llvm::ArrayType::get(Ty, 0);
      GV = CreateRuntimeVariable(PTy, str);
      // Decay array -> ptr
      ConstantStringClassRef = 
        llvm::ConstantExpr::getGetElementPtr(GV, Zeros);
    }
  }

  if (!NSConstantStringType) {
    // Construct the type for a constant NSString.
    RecordDecl *D = CreateRecordDecl(Context, TTK_Struct, 
                                     Context.getTranslationUnitDecl(),
                                   &Context.Idents.get("__builtin_NSString"));
    D->startDefinition();
      
    QualType FieldTypes[3];
    
    // const int *isa;
    FieldTypes[0] = Context.getPointerType(Context.IntTy.withConst());
    // const char *str;
    FieldTypes[1] = Context.getPointerType(Context.CharTy.withConst());
    // unsigned int length;
    FieldTypes[2] = Context.UnsignedIntTy;
    
    // Create fields
    for (unsigned i = 0; i < 3; ++i) {
      FieldDecl *Field = FieldDecl::Create(Context, D,
                                           SourceLocation(),
                                           SourceLocation(), 0,
                                           FieldTypes[i], /*TInfo=*/0,
                                           /*BitWidth=*/0,
                                           /*Mutable=*/false,
                                           /*HasInit=*/false);
      Field->setAccess(AS_public);
      D->addDecl(Field);
    }
    
    D->completeDefinition();
    QualType NSTy = Context.getTagDeclType(D);
    NSConstantStringType = cast<llvm::StructType>(getTypes().ConvertType(NSTy));
  }
  
  llvm::Constant *Fields[3];
  
  // Class pointer.
  Fields[0] = ConstantStringClassRef;
  
  // String pointer.
  llvm::Constant *C =
    llvm::ConstantDataArray::getString(VMContext, Entry.getKey());
  
  llvm::GlobalValue::LinkageTypes Linkage;
  bool isConstant;
  Linkage = llvm::GlobalValue::PrivateLinkage;
  isConstant = !LangOpts.WritableStrings;
  
  llvm::GlobalVariable *GV =
  new llvm::GlobalVariable(getModule(), C->getType(), isConstant, Linkage, C,
                           ".str");
  GV->setUnnamedAddr(true);
  CharUnits Align = getContext().getTypeAlignInChars(getContext().CharTy);
  GV->setAlignment(Align.getQuantity());
  Fields[1] = llvm::ConstantExpr::getGetElementPtr(GV, Zeros);
  
  // String length.
  llvm::Type *Ty = getTypes().ConvertType(getContext().UnsignedIntTy);
  Fields[2] = llvm::ConstantInt::get(Ty, StringLength);
  
  // The struct.
  C = llvm::ConstantStruct::get(NSConstantStringType, Fields);
  GV = new llvm::GlobalVariable(getModule(), C->getType(), true,
                                llvm::GlobalVariable::PrivateLinkage, C,
                                "_unnamed_nsstring_");
  // FIXME. Fix section.
  if (const char *Sect = 
        LangOpts.ObjCNonFragileABI 
          ? getContext().getTargetInfo().getNSStringNonFragileABISection() 
          : getContext().getTargetInfo().getNSStringSection())
    GV->setSection(Sect);
  Entry.setValue(GV);
  
  return GV;
}

QualType CodeGenModule::getObjCFastEnumerationStateType() {
  if (ObjCFastEnumerationStateType.isNull()) {
    RecordDecl *D = CreateRecordDecl(Context, TTK_Struct, 
                                     Context.getTranslationUnitDecl(),
                      &Context.Idents.get("__objcFastEnumerationState"));
    D->startDefinition();
    
    QualType FieldTypes[] = {
      Context.UnsignedLongTy,
      Context.getPointerType(Context.getObjCIdType()),
      Context.getPointerType(Context.UnsignedLongTy),
      Context.getConstantArrayType(Context.UnsignedLongTy,
                           llvm::APInt(32, 5), ArrayType::Normal, 0)
    };
    
    for (size_t i = 0; i < 4; ++i) {
      FieldDecl *Field = FieldDecl::Create(Context,
                                           D,
                                           SourceLocation(),
                                           SourceLocation(), 0,
                                           FieldTypes[i], /*TInfo=*/0,
                                           /*BitWidth=*/0,
                                           /*Mutable=*/false,
                                           /*HasInit=*/false);
      Field->setAccess(AS_public);
      D->addDecl(Field);
    }
    
    D->completeDefinition();
    ObjCFastEnumerationStateType = Context.getTagDeclType(D);
  }
  
  return ObjCFastEnumerationStateType;
}

llvm::Constant *
CodeGenModule::GetConstantArrayFromStringLiteral(const StringLiteral *E) {
  assert(!E->getType()->isPointerType() && "Strings are always arrays");
  
  // Don't emit it as the address of the string, emit the string data itself
  // as an inline array.
  if (E->getCharByteWidth() == 1) {
    SmallString<64> Str(E->getString());

    // Resize the string to the right size, which is indicated by its type.
    const ConstantArrayType *CAT = Context.getAsConstantArrayType(E->getType());
    Str.resize(CAT->getSize().getZExtValue());
    return llvm::ConstantDataArray::getString(VMContext, Str, false);
  }
  
  llvm::ArrayType *AType =
    cast<llvm::ArrayType>(getTypes().ConvertType(E->getType()));
  llvm::Type *ElemTy = AType->getElementType();
  unsigned NumElements = AType->getNumElements();

  // Wide strings have either 2-byte or 4-byte elements.
  if (ElemTy->getPrimitiveSizeInBits() == 16) {
    SmallVector<uint16_t, 32> Elements;
    Elements.reserve(NumElements);

    for(unsigned i = 0, e = E->getLength(); i != e; ++i)
      Elements.push_back(E->getCodeUnit(i));
    Elements.resize(NumElements);
    return llvm::ConstantDataArray::get(VMContext, Elements);
  }
  
  assert(ElemTy->getPrimitiveSizeInBits() == 32);
  SmallVector<uint32_t, 32> Elements;
  Elements.reserve(NumElements);
  
  for(unsigned i = 0, e = E->getLength(); i != e; ++i)
    Elements.push_back(E->getCodeUnit(i));
  Elements.resize(NumElements);
  return llvm::ConstantDataArray::get(VMContext, Elements);
}

/// GetAddrOfConstantStringFromLiteral - Return a pointer to a
/// constant array for the given string literal.
llvm::Constant *
CodeGenModule::GetAddrOfConstantStringFromLiteral(const StringLiteral *S) {
  CharUnits Align = getContext().getTypeAlignInChars(S->getType());
  if (S->isAscii() || S->isUTF8()) {
    SmallString<64> Str(S->getString());
    
    // Resize the string to the right size, which is indicated by its type.
    const ConstantArrayType *CAT = Context.getAsConstantArrayType(S->getType());
    Str.resize(CAT->getSize().getZExtValue());
    return GetAddrOfConstantString(Str, /*GlobalName*/ 0, Align.getQuantity());
  }

  // FIXME: the following does not memoize wide strings.
  llvm::Constant *C = GetConstantArrayFromStringLiteral(S);
  llvm::GlobalVariable *GV =
    new llvm::GlobalVariable(getModule(),C->getType(),
                             !LangOpts.WritableStrings,
                             llvm::GlobalValue::PrivateLinkage,
                             C,".str");

  GV->setAlignment(Align.getQuantity());
  GV->setUnnamedAddr(true);
  return GV;
}

/// GetAddrOfConstantStringFromObjCEncode - Return a pointer to a constant
/// array for the given ObjCEncodeExpr node.
llvm::Constant *
CodeGenModule::GetAddrOfConstantStringFromObjCEncode(const ObjCEncodeExpr *E) {
  std::string Str;
  getContext().getObjCEncodingForType(E->getEncodedType(), Str);

  return GetAddrOfConstantCString(Str);
}


/// GenerateWritableString -- Creates storage for a string literal.
static llvm::GlobalVariable *GenerateStringLiteral(StringRef str,
                                             bool constant,
                                             CodeGenModule &CGM,
                                             const char *GlobalName,
                                             unsigned Alignment) {
  // Create Constant for this string literal. Don't add a '\0'.
  llvm::Constant *C =
      llvm::ConstantDataArray::getString(CGM.getLLVMContext(), str, false);

  // Create a global variable for this string
  llvm::GlobalVariable *GV =
    new llvm::GlobalVariable(CGM.getModule(), C->getType(), constant,
                             llvm::GlobalValue::PrivateLinkage,
                             C, GlobalName);
  GV->setAlignment(Alignment);
  GV->setUnnamedAddr(true);
  return GV;
}

/// GetAddrOfConstantString - Returns a pointer to a character array
/// containing the literal. This contents are exactly that of the
/// given string, i.e. it will not be null terminated automatically;
/// see GetAddrOfConstantCString. Note that whether the result is
/// actually a pointer to an LLVM constant depends on
/// Feature.WriteableStrings.
///
/// The result has pointer to array type.
llvm::Constant *CodeGenModule::GetAddrOfConstantString(StringRef Str,
                                                       const char *GlobalName,
                                                       unsigned Alignment) {
  // Get the default prefix if a name wasn't specified.
  if (!GlobalName)
    GlobalName = ".str";

  // Don't share any string literals if strings aren't constant.
  if (LangOpts.WritableStrings)
    return GenerateStringLiteral(Str, false, *this, GlobalName, Alignment);

  llvm::StringMapEntry<llvm::GlobalVariable *> &Entry =
    ConstantStringMap.GetOrCreateValue(Str);

  if (llvm::GlobalVariable *GV = Entry.getValue()) {
    if (Alignment > GV->getAlignment()) {
      GV->setAlignment(Alignment);
    }
    return GV;
  }

  // Create a global variable for this.
  llvm::GlobalVariable *GV = GenerateStringLiteral(Str, true, *this, GlobalName,
                                                   Alignment);
  Entry.setValue(GV);
  return GV;
}

/// GetAddrOfConstantCString - Returns a pointer to a character
/// array containing the literal and a terminating '\0'
/// character. The result has pointer to array type.
llvm::Constant *CodeGenModule::GetAddrOfConstantCString(const std::string &Str,
                                                        const char *GlobalName,
                                                        unsigned Alignment) {
  StringRef StrWithNull(Str.c_str(), Str.size() + 1);
  return GetAddrOfConstantString(StrWithNull, GlobalName, Alignment);
}

/// EmitObjCPropertyImplementations - Emit information for synthesized
/// properties for an implementation.
void CodeGenModule::EmitObjCPropertyImplementations(const
                                                    ObjCImplementationDecl *D) {
  for (ObjCImplementationDecl::propimpl_iterator
         i = D->propimpl_begin(), e = D->propimpl_end(); i != e; ++i) {
    ObjCPropertyImplDecl *PID = &*i;

    // Dynamic is just for type-checking.
    if (PID->getPropertyImplementation() == ObjCPropertyImplDecl::Synthesize) {
      ObjCPropertyDecl *PD = PID->getPropertyDecl();

      // Determine which methods need to be implemented, some may have
      // been overridden. Note that ::isSynthesized is not the method
      // we want, that just indicates if the decl came from a
      // property. What we want to know is if the method is defined in
      // this implementation.
      if (!D->getInstanceMethod(PD->getGetterName()))
        CodeGenFunction(*this).GenerateObjCGetter(
                                 const_cast<ObjCImplementationDecl *>(D), PID);
      if (!PD->isReadOnly() &&
          !D->getInstanceMethod(PD->getSetterName()))
        CodeGenFunction(*this).GenerateObjCSetter(
                                 const_cast<ObjCImplementationDecl *>(D), PID);
    }
  }
}

static bool needsDestructMethod(ObjCImplementationDecl *impl) {
  const ObjCInterfaceDecl *iface = impl->getClassInterface();
  for (const ObjCIvarDecl *ivar = iface->all_declared_ivar_begin();
       ivar; ivar = ivar->getNextIvar())
    if (ivar->getType().isDestructedType())
      return true;

  return false;
}

/// EmitObjCIvarInitializations - Emit information for ivar initialization
/// for an implementation.
void CodeGenModule::EmitObjCIvarInitializations(ObjCImplementationDecl *D) {
  // We might need a .cxx_destruct even if we don't have any ivar initializers.
  if (needsDestructMethod(D)) {
    IdentifierInfo *II = &getContext().Idents.get(".cxx_destruct");
    Selector cxxSelector = getContext().Selectors.getSelector(0, &II);
    ObjCMethodDecl *DTORMethod =
      ObjCMethodDecl::Create(getContext(), D->getLocation(), D->getLocation(),
                             cxxSelector, getContext().VoidTy, 0, D,
                             /*isInstance=*/true, /*isVariadic=*/false,
                          /*isSynthesized=*/true, /*isImplicitlyDeclared=*/true,
                             /*isDefined=*/false, ObjCMethodDecl::Required);
    D->addInstanceMethod(DTORMethod);
    CodeGenFunction(*this).GenerateObjCCtorDtorMethod(D, DTORMethod, false);
    D->setHasCXXStructors(true);
  }

  // If the implementation doesn't have any ivar initializers, we don't need
  // a .cxx_construct.
  if (D->getNumIvarInitializers() == 0)
    return;
  
  IdentifierInfo *II = &getContext().Idents.get(".cxx_construct");
  Selector cxxSelector = getContext().Selectors.getSelector(0, &II);
  // The constructor returns 'self'.
  ObjCMethodDecl *CTORMethod = ObjCMethodDecl::Create(getContext(), 
                                                D->getLocation(),
                                                D->getLocation(),
                                                cxxSelector,
                                                getContext().getObjCIdType(), 0, 
                                                D, /*isInstance=*/true,
                                                /*isVariadic=*/false,
                                                /*isSynthesized=*/true,
                                                /*isImplicitlyDeclared=*/true,
                                                /*isDefined=*/false,
                                                ObjCMethodDecl::Required);
  D->addInstanceMethod(CTORMethod);
  CodeGenFunction(*this).GenerateObjCCtorDtorMethod(D, CTORMethod, true);
  D->setHasCXXStructors(true);
}

/// EmitNamespace - Emit all declarations in a namespace.
void CodeGenModule::EmitNamespace(const NamespaceDecl *ND) {
  for (RecordDecl::decl_iterator I = ND->decls_begin(), E = ND->decls_end();
       I != E; ++I)
    EmitTopLevelDecl(*I);
}

// EmitLinkageSpec - Emit all declarations in a linkage spec.
void CodeGenModule::EmitLinkageSpec(const LinkageSpecDecl *LSD) {
  if (LSD->getLanguage() != LinkageSpecDecl::lang_c &&
      LSD->getLanguage() != LinkageSpecDecl::lang_cxx) {
    ErrorUnsupported(LSD, "linkage spec");
    return;
  }

  for (RecordDecl::decl_iterator I = LSD->decls_begin(), E = LSD->decls_end();
       I != E; ++I)
    EmitTopLevelDecl(*I);
}

/// EmitTopLevelDecl - Emit code for a single top level declaration.
void CodeGenModule::EmitTopLevelDecl(Decl *D) {
  // If an error has occurred, stop code generation, but continue
  // parsing and semantic analysis (to ensure all warnings and errors
  // are emitted).
  if (Diags.hasErrorOccurred())
    return;

  // Ignore dependent declarations.
  if (D->getDeclContext() && D->getDeclContext()->isDependentContext())
    return;

  switch (D->getKind()) {
  case Decl::CXXConversion:
  case Decl::CXXMethod:
  case Decl::Function:
    // Skip function templates
    if (cast<FunctionDecl>(D)->getDescribedFunctionTemplate() ||
        cast<FunctionDecl>(D)->isLateTemplateParsed())
      return;

    EmitGlobal(cast<FunctionDecl>(D));
    break;
      
  case Decl::Var:
    EmitGlobal(cast<VarDecl>(D));
    break;

  // Indirect fields from global anonymous structs and unions can be
  // ignored; only the actual variable requires IR gen support.
  case Decl::IndirectField:
    break;

  // C++ Decls
  case Decl::Namespace:
    EmitNamespace(cast<NamespaceDecl>(D));
    break;
    // No code generation needed.
  case Decl::UsingShadow:
  case Decl::Using:
  case Decl::UsingDirective:
  case Decl::ClassTemplate:
  case Decl::FunctionTemplate:
  case Decl::TypeAliasTemplate:
  case Decl::NamespaceAlias:
  case Decl::Block:
  case Decl::Import:
    break;
  case Decl::CXXConstructor:
    // Skip function templates
    if (cast<FunctionDecl>(D)->getDescribedFunctionTemplate() ||
        cast<FunctionDecl>(D)->isLateTemplateParsed())
      return;
      
    EmitCXXConstructors(cast<CXXConstructorDecl>(D));
    break;
  case Decl::CXXDestructor:
    if (cast<FunctionDecl>(D)->isLateTemplateParsed())
      return;
    EmitCXXDestructors(cast<CXXDestructorDecl>(D));
    break;

  case Decl::StaticAssert:
    // Nothing to do.
    break;

  // Objective-C Decls

  // Forward declarations, no (immediate) code generation.
  case Decl::ObjCInterface:
    break;
  
  case Decl::ObjCCategory: {
    ObjCCategoryDecl *CD = cast<ObjCCategoryDecl>(D);
    if (CD->IsClassExtension() && CD->hasSynthBitfield())
      Context.ResetObjCLayout(CD->getClassInterface());
    break;
  }

  case Decl::ObjCProtocol: {
    ObjCProtocolDecl *Proto = cast<ObjCProtocolDecl>(D);
    if (Proto->isThisDeclarationADefinition())
      ObjCRuntime->GenerateProtocol(Proto);
    break;
  }
      
  case Decl::ObjCCategoryImpl:
    // Categories have properties but don't support synthesize so we
    // can ignore them here.
    ObjCRuntime->GenerateCategory(cast<ObjCCategoryImplDecl>(D));
    break;

  case Decl::ObjCImplementation: {
    ObjCImplementationDecl *OMD = cast<ObjCImplementationDecl>(D);
    if (LangOpts.ObjCNonFragileABI2 && OMD->hasSynthBitfield())
      Context.ResetObjCLayout(OMD->getClassInterface());
    EmitObjCPropertyImplementations(OMD);
    EmitObjCIvarInitializations(OMD);
    ObjCRuntime->GenerateClass(OMD);
    // Emit global variable debug information.
    if (CGDebugInfo *DI = getModuleDebugInfo())
      DI->getOrCreateInterfaceType(getContext().getObjCInterfaceType(OMD->getClassInterface()),
				   OMD->getLocation());
    
    break;
  }
  case Decl::ObjCMethod: {
    ObjCMethodDecl *OMD = cast<ObjCMethodDecl>(D);
    // If this is not a prototype, emit the body.
    if (OMD->getBody())
      CodeGenFunction(*this).GenerateObjCMethod(OMD);
    break;
  }
  case Decl::ObjCCompatibleAlias:
    ObjCRuntime->RegisterAlias(cast<ObjCCompatibleAliasDecl>(D));
    break;

  case Decl::LinkageSpec:
    EmitLinkageSpec(cast<LinkageSpecDecl>(D));
    break;

  case Decl::FileScopeAsm: {
    FileScopeAsmDecl *AD = cast<FileScopeAsmDecl>(D);
    StringRef AsmString = AD->getAsmString()->getString();

    const std::string &S = getModule().getModuleInlineAsm();
    if (S.empty())
      getModule().setModuleInlineAsm(AsmString);
    else if (*--S.end() == '\n')
      getModule().setModuleInlineAsm(S + AsmString.str());
    else
      getModule().setModuleInlineAsm(S + '\n' + AsmString.str());
    break;
  }

  default:
    // Make sure we handled everything we should, every other kind is a
    // non-top-level decl.  FIXME: Would be nice to have an isTopLevelDeclKind
    // function. Need to recode Decl::Kind to do that easily.
    assert(isa<TypeDecl>(D) && "Unsupported decl kind");
  }
}

/// Turns the given pointer into a constant.
static llvm::Constant *GetPointerConstant(llvm::LLVMContext &Context,
                                          const void *Ptr) {
  uintptr_t PtrInt = reinterpret_cast<uintptr_t>(Ptr);
  llvm::Type *i64 = llvm::Type::getInt64Ty(Context);
  return llvm::ConstantInt::get(i64, PtrInt);
}

static void EmitGlobalDeclMetadata(CodeGenModule &CGM,
                                   llvm::NamedMDNode *&GlobalMetadata,
                                   GlobalDecl D,
                                   llvm::GlobalValue *Addr) {
  if (!GlobalMetadata)
    GlobalMetadata =
      CGM.getModule().getOrInsertNamedMetadata("clang.global.decl.ptrs");

  // TODO: should we report variant information for ctors/dtors?
  llvm::Value *Ops[] = {
    Addr,
    GetPointerConstant(CGM.getLLVMContext(), D.getDecl())
  };
  GlobalMetadata->addOperand(llvm::MDNode::get(CGM.getLLVMContext(), Ops));
}

/// Emits metadata nodes associating all the global values in the
/// current module with the Decls they came from.  This is useful for
/// projects using IR gen as a subroutine.
///
/// Since there's currently no way to associate an MDNode directly
/// with an llvm::GlobalValue, we create a global named metadata
/// with the name 'clang.global.decl.ptrs'.
void CodeGenModule::EmitDeclMetadata() {
  llvm::NamedMDNode *GlobalMetadata = 0;

  // StaticLocalDeclMap
  for (llvm::DenseMap<GlobalDecl,StringRef>::iterator
         I = MangledDeclNames.begin(), E = MangledDeclNames.end();
       I != E; ++I) {
    llvm::GlobalValue *Addr = getModule().getNamedValue(I->second);
    EmitGlobalDeclMetadata(*this, GlobalMetadata, I->first, Addr);
  }
}

/// Emits metadata nodes for all the local variables in the current
/// function.
void CodeGenFunction::EmitDeclMetadata() {
  if (LocalDeclMap.empty()) return;

  llvm::LLVMContext &Context = getLLVMContext();

  // Find the unique metadata ID for this name.
  unsigned DeclPtrKind = Context.getMDKindID("clang.decl.ptr");

  llvm::NamedMDNode *GlobalMetadata = 0;

  for (llvm::DenseMap<const Decl*, llvm::Value*>::iterator
         I = LocalDeclMap.begin(), E = LocalDeclMap.end(); I != E; ++I) {
    const Decl *D = I->first;
    llvm::Value *Addr = I->second;

    if (llvm::AllocaInst *Alloca = dyn_cast<llvm::AllocaInst>(Addr)) {
      llvm::Value *DAddr = GetPointerConstant(getLLVMContext(), D);
      Alloca->setMetadata(DeclPtrKind, llvm::MDNode::get(Context, DAddr));
    } else if (llvm::GlobalValue *GV = dyn_cast<llvm::GlobalValue>(Addr)) {
      GlobalDecl GD = GlobalDecl(cast<VarDecl>(D));
      EmitGlobalDeclMetadata(CGM, GlobalMetadata, GD, GV);
    }
  }
}

void CodeGenModule::EmitCoverageFile() {
  if (!getCodeGenOpts().CoverageFile.empty()) {
    if (llvm::NamedMDNode *CUNode = TheModule.getNamedMetadata("llvm.dbg.cu")) {
      llvm::NamedMDNode *GCov = TheModule.getOrInsertNamedMetadata("llvm.gcov");
      llvm::LLVMContext &Ctx = TheModule.getContext();
      llvm::MDString *CoverageFile =
          llvm::MDString::get(Ctx, getCodeGenOpts().CoverageFile);
      for (int i = 0, e = CUNode->getNumOperands(); i != e; ++i) {
        llvm::MDNode *CU = CUNode->getOperand(i);
        llvm::Value *node[] = { CoverageFile, CU };
        llvm::MDNode *N = llvm::MDNode::get(Ctx, node);
        GCov->addOperand(N);
      }
    }
  }
}
