/*******************************************************************************
 * Copyright (c) 2000, 2020 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "codegen/AheadOfTimeCompile.hpp"
#include "codegen/ARMAOTRelocation.hpp"
#include "compile/SymbolReferenceTable.hpp"
#include "codegen/CodeGenerator.hpp"
#include "env/FrontEnd.hpp"
#include "codegen/Instruction.hpp"
#include "compile/AOTClassInfo.hpp"
#include "compile/Compilation.hpp"
#include "compile/ResolvedMethod.hpp"
#include "compile/VirtualGuard.hpp"
#include "env/CHTable.hpp"
#include "env/ClassLoaderTable.hpp"
#include "env/SharedCache.hpp"
#include "env/jittypes.h"
#include "env/VMJ9.h"
#include "il/LabelSymbol.hpp"
#include "il/Node.hpp"
#include "il/Node_inlines.hpp"
#include "il/StaticSymbol.hpp"
#include "il/SymbolReference.hpp"
#include "runtime/RelocationRuntime.hpp"
#include "runtime/RelocationRecord.hpp"

#define  NON_HELPER         0

J9::ARM::AheadOfTimeCompile::AheadOfTimeCompile(TR::CodeGenerator *cg)
   : J9::AheadOfTimeCompile(_relocationTargetTypeToHeaderSizeMap, cg->comp()),
     _cg(cg),
     _relocationList(self()->trMemory())
   {
   }

void J9::ARM::AheadOfTimeCompile::processRelocations()
   {
   TR_J9VMBase *fej9 = (TR_J9VMBase *)(self()->cg()->fe());
   ListIterator<TR::ARMRelocation>  iterator(&self()->getRelocationList());
   TR::ARMRelocation               *relocation;
   TR::IteratedExternalRelocation  *r;

   for (relocation=iterator.getFirst();
        relocation!=NULL;
        relocation=iterator.getNext())
      {
      relocation->mapRelocation(self()->cg());
      }

   for (auto aotIterator = self()->cg()->getExternalRelocationList().begin(); aotIterator != self()->cg()->getExternalRelocationList().end(); ++aotIterator)
      {
	  (*aotIterator)->addExternalRelocation(self()->cg());
      }

   for (r = self()->getAOTRelocationTargets().getFirst();
        r != NULL;
        r = r->getNext())
      {
      self()->addToSizeOfAOTRelocations(r->getSizeOfRelocationData());
      }

   // now allocate the memory  size of all iterated relocations + the header (total length field)

   if (self()->getSizeOfAOTRelocations() != 0)
      {
      uint8_t *relocationDataCursor = self()->setRelocationData(fej9->allocateRelocationData(self()->comp(), self()->getSizeOfAOTRelocations() + 4));

      // set up the size for the region
      *(uint32_t *)relocationDataCursor = self()->getSizeOfAOTRelocations() + 4;
      relocationDataCursor += 4;

      // set up pointers for each iterated relocation and initialize header
      TR::IteratedExternalRelocation *s;
      for (s = self()->getAOTRelocationTargets().getFirst();
           s != NULL;
           s = s->getNext())
         {
         s->setRelocationData(relocationDataCursor);
         s->initializeRelocation(_cg);
         relocationDataCursor += s->getSizeOfRelocationData();
         }
      }
   }

uint8_t *J9::ARM::AheadOfTimeCompile::initializeAOTRelocationHeader(TR::IteratedExternalRelocation *relocation)
   {
   TR::Compilation* comp = TR::comp();
   TR_J9VMBase *fej9 = (TR_J9VMBase *)(_cg->fe());
   TR_SharedCache *sharedCache = fej9->sharedCache();

   TR_VirtualGuard *guard;
   uint8_t flags = 0;
   TR_ResolvedMethod *resolvedMethod;

   uint8_t *cursor = relocation->getRelocationData();

   TR_RelocationRuntime *reloRuntime = comp->reloRuntime();
   TR_RelocationTarget *reloTarget = reloRuntime->reloTarget();

   uint8_t * aotMethodCodeStart = (uint8_t *) comp->getRelocatableMethodCodeStart();

   // size of relocation goes first in all types
   *(uint16_t *)cursor = relocation->getSizeOfRelocationData();
   cursor += 2;

   uint8_t  modifier = 0;
   uint8_t *relativeBitCursor = cursor;

   if (relocation->needsWideOffsets())
      modifier |= RELOCATION_TYPE_WIDE_OFFSET;
   *cursor++ = (uint8_t)(relocation->getTargetKind());
   uint8_t *flagsCursor = cursor++;
   *flagsCursor = modifier;
   uint32_t *wordAfterHeader = (uint32_t*)cursor;

   // This has to be created after the kind has been written into the header
   TR_RelocationRecord storage;
   TR_RelocationRecord *reloRecord = TR_RelocationRecord::create(&storage, reloRuntime, reloTarget, reinterpret_cast<TR_RelocationRecordBinaryTemplate *>(relocation->getRelocationData()));

   switch (relocation->getTargetKind())
      {
      case TR_MethodObject:
         {
         TR_RelocationRecordInformation *recordInfo = (TR_RelocationRecordInformation*) relocation->getTargetAddress();
         TR::SymbolReference *tempSR = (TR::SymbolReference *) recordInfo->data1;
         uintptr_t inlinedSiteIndex = self()->findCorrectInlinedSiteIndex(tempSR->getOwningMethod(comp)->constantPool(), recordInfo->data2);
         uint8_t flags = (uint8_t) recordInfo->data3;//Sequence ID
         //TODO
         *(uintptr_t *)cursor = inlinedSiteIndex;
         cursor += SIZEPOINTER;
         // next word is the address of the constant pool to
         // which the index refers
         *(uintptr_t *)cursor =
            (uintptr_t)tempSR->getOwningMethod(TR::comp())->constantPool();
         cursor += SIZEPOINTER;
         }
         break;
      case TR_JNISpecialTargetAddress:
      case TR_VirtualRamMethodConst:
         {
         TR::SymbolReference *tempSR = (TR::SymbolReference *)relocation->getTargetAddress();
         uintptr_t inlinedSiteIndex = (uintptr_t)relocation->getTargetAddress2();

         inlinedSiteIndex = self()->findCorrectInlinedSiteIndex(tempSR->getOwningMethod(comp)->constantPool(), inlinedSiteIndex);

         *(uintptr_t *)cursor = inlinedSiteIndex;  // inlinedSiteIndex
         cursor += SIZEPOINTER;

         *(uintptr_t *)cursor = (uintptr_t)tempSR->getOwningMethod(comp)->constantPool(); // constantPool
         cursor += SIZEPOINTER;

         uintptr_t cpIndex=(uintptr_t)tempSR->getCPIndex();
         *(uintptr_t *)cursor =cpIndex;// cpIndex
         cursor += SIZEPOINTER;
         }
         break;
      case TR_ClassAddress:
         {
         TR_RelocationRecordInformation *recordInfo = (TR_RelocationRecordInformation*) relocation->getTargetAddress();
         TR::SymbolReference *tempSR = (TR::SymbolReference *) recordInfo->data1;

         // These flags are unused at the moment. If they're needed later,
         // they'll be needed for TR_ArbitraryClassAddress as well
         uint8_t flags = (uint8_t) recordInfo->data3;

         uintptr_t inlinedSiteIndex = self()->findCorrectInlinedSiteIndex(tempSR->getOwningMethod(comp)->constantPool(), recordInfo->data2);

         *(uintptr_t *)cursor = inlinedSiteIndex; // inlinedSiteIndex

         cursor += SIZEPOINTER;

         *(uintptr_t *)cursor = (uintptr_t)tempSR->getOwningMethod(comp)->constantPool(); // constantPool
         cursor += SIZEPOINTER;

         *(uintptr_t *)cursor = tempSR->getCPIndex(); // cpIndex
         cursor += SIZEPOINTER;
         }
         break;
      case TR_DataAddress:
         {
         TR_RelocationRecordInformation *recordInfo = (TR_RelocationRecordInformation *) relocation->getTargetAddress();
         TR::SymbolReference *tempSR = (TR::SymbolReference *) recordInfo->data1;
         uintptr_t inlinedSiteIndex = self()->findCorrectInlinedSiteIndex(tempSR->getOwningMethod(comp)->constantPool(), recordInfo->data2);
         uint8_t flags = (uint8_t) recordInfo->data3;
         TR_ASSERT((flags & RELOCATION_CROSS_PLATFORM_FLAGS_MASK) == 0,  "reloFlags bits overlap cross-platform flags bits\n");
         *flagsCursor |= (flags & RELOCATION_RELOC_FLAGS_MASK);

         // relocation target
         *(uintptr_t *) cursor = inlinedSiteIndex;
         cursor += SIZEPOINTER;

         *(uintptr_t *) cursor = (uintptr_t) tempSR->getOwningMethod(comp)->constantPool(); // constantPool
         cursor += SIZEPOINTER;

         *(uintptr_t *) cursor = tempSR->getCPIndex(); // cpIndex
         cursor += SIZEPOINTER;

         *(uintptr_t *) cursor = tempSR->getOffset(); // offset
         cursor += SIZEPOINTER;

         break;
         }
      case TR_FixedSequenceAddress2:
         {
         uint8_t flags = (uint8_t) ((uintptr_t) relocation->getTargetAddress2());
         TR_ASSERT((flags & RELOCATION_CROSS_PLATFORM_FLAGS_MASK) == 0,  "reloFlags bits overlap cross-platform flags bits\n");
         *flagsCursor |= (flags & RELOCATION_RELOC_FLAGS_MASK);

         TR_ASSERT(relocation->getTargetAddress(), "target address is NULL");
         *(uint32_t *) cursor = relocation->getTargetAddress() ?
                              (uint32_t)((uint8_t *) relocation->getTargetAddress() - aotMethodCodeStart) : 0x0;
         cursor += 4;
         }
         break;
      case TR_AbsoluteMethodAddressOrderedPair:
         break;
      case TR_BodyInfoAddressLoad:
         {
         uint8_t flags = (uint8_t) ((uintptr_t) relocation->getTargetAddress2());// sequence ID
         TR_ASSERT((flags & RELOCATION_CROSS_PLATFORM_FLAGS_MASK) == 0,  "reloFlags bits overlap cross-platform flags bits\n");
         *flagsCursor |= (flags & RELOCATION_RELOC_FLAGS_MASK);
         break;
         }
      case TR_ConstantPoolOrderedPair:
         {
         *(uintptr_t *)cursor = (uintptr_t)relocation->getTargetAddress2(); // inlined site index
         cursor += SIZEPOINTER;

         *(uintptr_t *)cursor = (uintptr_t)relocation->getTargetAddress(); // constantPool
         cursor += SIZEPOINTER;
         break;
         }

      case TR_J2IVirtualThunkPointer:
         {
         auto info = (TR_RelocationRecordInformation*)relocation->getTargetAddress();

         *(uintptr_t *)cursor = (uintptr_t)info->data2; // inlined site index
         cursor += SIZEPOINTER;

         *(uintptr_t *)cursor = (uintptr_t)info->data1; // constantPool
         cursor += SIZEPOINTER;

         *(uintptr_t *)cursor = (uintptr_t)info->data3; // offset to J2I virtual thunk pointer
         cursor += SIZEPOINTER;
         }
         break;

      case TR_CheckMethodExit:
         {
         *(uintptr_t*)cursor = (uintptr_t)relocation->getTargetAddress();
         cursor += SIZEPOINTER;
         }
        break;
      case TR_J2IThunks:
         {
         // Note: thunk relos should only be created for 64 bit
         // *(uintptr_t *)cursor = (uintptr_t)relocation->getTargetAddress2(); // inlined site index

         TR::Node *node = (TR::Node*)relocation->getTargetAddress();
         TR::SymbolReference *symRef = node->getSymbolReference();

         *(uintptr_t *)cursor = (uintptr_t)node->getInlinedSiteIndex();
         cursor += SIZEPOINTER;

         *(uintptr_t *)cursor = (uintptr_t)symRef->getOwningMethod(comp)->constantPool(); // cp address
         cursor += SIZEPOINTER;

         *(uintptr_t *)cursor = (uintptr_t)symRef->getCPIndex(); // cp index
         cursor += SIZEPOINTER;

         break;
         }

      case TR_RamMethodSequence:
      case TR_RamMethodSequenceReg:
         {
         uint8_t flags = (uint8_t) ((uintptr_t) relocation->getTargetAddress2());// sequence ID
         TR_ASSERT((flags & RELOCATION_CROSS_PLATFORM_FLAGS_MASK) == 0,  "reloFlags bits overlap cross-platform flags bits\n");
         *flagsCursor |= (flags & RELOCATION_RELOC_FLAGS_MASK);
         cursor += SIZEPOINTER; //advance pointer (skip offset)
         }
         break;

      case TR_GlobalValue:
         {
         uint8_t flags = (uint8_t) ((uintptr_t) relocation->getTargetAddress2());// sequence ID
         TR_ASSERT((flags & RELOCATION_CROSS_PLATFORM_FLAGS_MASK) == 0,  "reloFlags bits overlap cross-platform flags bits\n");
         *flagsCursor |= (flags & RELOCATION_RELOC_FLAGS_MASK);
         *(uintptr_t*) cursor = (uintptr_t)relocation->getTargetAddress();

         cursor += SIZEPOINTER;
         break;
         }

      case TR_ArbitraryClassAddress:
         {
         // ExternalRelocation data is as expected for TR_ClassAddress
         auto recordInfo =
            (TR_RelocationRecordInformation*)relocation->getTargetAddress();

         auto symRef = (TR::SymbolReference *)recordInfo->data1;
         auto sym = symRef->getSymbol()->castToStaticSymbol();
         auto j9class = (TR_OpaqueClassBlock *)sym->getStaticAddress();
         // flags stored in data3 are currently unused
         uintptr_t inlinedSiteIndex = self()->findCorrectInlinedSiteIndex(symRef->getOwningMethod(comp)->constantPool(), recordInfo->data2);

         // Data identifying the class is as though for TR_ClassPointer
         // (TR_RelocationRecordPointerBinaryTemplate)
         *(uintptr_t *)cursor = inlinedSiteIndex;
         cursor += SIZEPOINTER;

         uintptr_t classChainOffsetInSharedCache = sharedCache->getClassChainOffsetOfIdentifyingLoaderForClazzInSharedCache(j9class);
         *(uintptr_t *)cursor = classChainOffsetInSharedCache;
         cursor += SIZEPOINTER;

         cursor = self()->emitClassChainOffset(cursor, j9class);
         }
         break;

      case TR_InlinedVirtualMethod:
      case TR_InlinedInterfaceMethod:
         {
         guard = (TR_VirtualGuard *) relocation->getTargetAddress2();

         // Setup flags field with type of method that needs to be validated at relocation time
         if (guard->getSymbolReference()->getSymbol()->getMethodSymbol()->isStatic())
            flags = inlinedMethodIsStatic;
         if (guard->getSymbolReference()->getSymbol()->getMethodSymbol()->isSpecial())
            flags = inlinedMethodIsSpecial;
         if (guard->getSymbolReference()->getSymbol()->getMethodSymbol()->isVirtual())
            flags = inlinedMethodIsVirtual;

         TR_ASSERT((flags & RELOCATION_CROSS_PLATFORM_FLAGS_MASK) == 0,  "reloFlags bits overlap cross-platform flags bits\n");
         *flagsCursor |= (flags & RELOCATION_RELOC_FLAGS_MASK);

         int32_t inlinedSiteIndex = guard->getCurrentInlinedSiteIndex();
         *(uintptr_t *) cursor = (uintptr_t) inlinedSiteIndex;
         cursor += SIZEPOINTER;

         *(uintptr_t *) cursor = (uintptr_t) guard->getSymbolReference()->getOwningMethod(comp)->constantPool(); // record constant pool
         cursor += SIZEPOINTER;

         *(uintptr_t*) cursor = (uintptr_t) guard->getSymbolReference()->getCPIndex(); // record cpIndex
         cursor += SIZEPOINTER;

         if (relocation->getTargetKind() == TR_InlinedInterfaceMethodWithNopGuard ||
             relocation->getTargetKind() == TR_InlinedInterfaceMethod)
            {
            TR_InlinedCallSite *inlinedCallSite = &comp->getInlinedCallSite(inlinedSiteIndex);
            TR_AOTMethodInfo *aotMethodInfo = (TR_AOTMethodInfo *) inlinedCallSite->_methodInfo;
            resolvedMethod = aotMethodInfo->resolvedMethod;
            }
         else
            {
            resolvedMethod = guard->getSymbolReference()->getSymbol()->getResolvedMethodSymbol()->getResolvedMethod();
            }

         TR_OpaqueClassBlock *inlinedMethodClass = resolvedMethod->containingClass();
         void *romClass = (void *) fej9->getPersistentClassPointerFromClassPointer(inlinedMethodClass);
         uintptr_t romClassOffsetInSharedCache = self()->offsetInSharedCacheFromPointer(sharedCache, romClass);

         *(uintptr_t *) cursor = romClassOffsetInSharedCache;
         cursor += SIZEPOINTER;

         if (relocation->getTargetKind() != TR_InlinedInterfaceMethod &&
             relocation->getTargetKind() != TR_InlinedVirtualMethod)
            {
            *(uintptr_t *) cursor = (uintptr_t) relocation->getTargetAddress(); // record Patch Destination Address
            cursor += SIZEPOINTER;
            }
         }
         break;

      case TR_ValidateArbitraryClass:
         {
         TR::AOTClassInfo *aotCI = (TR::AOTClassInfo*) relocation->getTargetAddress2();
         TR_OpaqueClassBlock *classToValidate = aotCI->_clazz;

         //store the classchain's offset for the classloader for the class
         uintptr_t classChainOffsetInSharedCacheForCL = sharedCache->getClassChainOffsetOfIdentifyingLoaderForClazzInSharedCache(classToValidate);
         *(uintptr_t *)cursor = classChainOffsetInSharedCacheForCL;
         cursor += SIZEPOINTER;

         //store the classchain's offset for the class that needs to be validated in the second run
         void *romClass = (void *)fej9->getPersistentClassPointerFromClassPointer(classToValidate);
         uintptr_t *classChainForClassToValidate = (uintptr_t *) aotCI->_classChain;
         uintptr_t classChainOffsetInSharedCache = self()->offsetInSharedCacheFromPointer(sharedCache, classChainForClassToValidate);
         *(uintptr_t *)cursor = classChainOffsetInSharedCache;
         cursor += SIZEPOINTER;
         }
         break;

      case TR_HCR:
         {
         flags = 0;
         if (((TR_HCRAssumptionFlags)((uintptr_t)(relocation->getTargetAddress2()))) == needsFullSizeRuntimeAssumption)
        	 flags = needsFullSizeRuntimeAssumption;
         TR_ASSERT((flags & RELOCATION_CROSS_PLATFORM_FLAGS_MASK) == 0,  "reloFlags bits overlap cross-platform flags bits\n");
         *flagsCursor |= (flags & RELOCATION_RELOC_FLAGS_MASK);

         *(uintptr_t*) cursor = (uintptr_t) relocation->getTargetAddress();
         cursor += SIZEPOINTER;
         }
         break;

      case TR_DebugCounter:
         {
         TR::DebugCounterBase *counter = (TR::DebugCounterBase *) relocation->getTargetAddress();
         if (!counter || !counter->getReloData() || !counter->getName())
            comp->failCompilation<TR::CompilationException>("Failed to generate debug counter relo data");

         TR::DebugCounterReloData *counterReloData = counter->getReloData();

         uintptr_t offset = (uintptr_t)fej9->sharedCache()->rememberDebugCounterName(counter->getName());

         uint8_t flags = (uint8_t)counterReloData->_seqKind;
         TR_ASSERT((flags & RELOCATION_CROSS_PLATFORM_FLAGS_MASK) == 0,  "reloFlags bits overlap cross-platform flags bits\n");
         *flagsCursor |= (flags & RELOCATION_RELOC_FLAGS_MASK);

         *(uintptr_t *)cursor = (uintptr_t)counterReloData->_callerIndex;
         cursor += SIZEPOINTER;
         *(uintptr_t *)cursor = (uintptr_t)counterReloData->_bytecodeIndex;
         cursor += SIZEPOINTER;
         *(uintptr_t *)cursor = offset;
         cursor += SIZEPOINTER;
         *(uintptr_t *)cursor = (uintptr_t)counterReloData->_delta;
         cursor += SIZEPOINTER;
         *(uintptr_t *)cursor = (uintptr_t)counterReloData->_fidelity;
         cursor += SIZEPOINTER;
         *(uintptr_t *)cursor = (uintptr_t)counterReloData->_staticDelta;
         cursor += SIZEPOINTER;
         }
         break;

      default:
         // initializeCommonAOTRelocationHeader is currently in the process
         // of becoming the canonical place to initialize the platform agnostic
         // relocation headers; new relocation records' header should be
         // initialized here.
         cursor = self()->initializeCommonAOTRelocationHeader(relocation, reloRecord);

      }
      return cursor;
   }


uint32_t J9::ARM::AheadOfTimeCompile::_relocationTargetTypeToHeaderSizeMap[TR_NumExternalRelocationKinds] =
   {
   12,                                       // TR_ConstantPool                        = 0
   8,                                        // TR_HelperAddress                       = 1
   12,                                       // TR_RelativeMethodAddress               = 2
   4,                                        // TR_AbsoluteMethodAddress               = 3
   20,                                       // TR_DataAddress                         = 4
   12,                                       // TR_ClassObject                         = 5
   12,                                       // TR_MethodObject                        = 6
   12,                                       // TR_InterfaceObject                     = 7
   8,                                        // TR_AbsoluteHelperAddress               = 8
   8,                                        // TR_FixedSequenceAddress                = 9
   8,                                        // TR_FixedSequenceAddress2               = 10
   16,                                       // TR_JNIVirtualTargetAddress             = 11
   16,                                       // TR_JNIStaticTargetAddress              = 12
   4,                                        // TR_ArrayCopyHelper                     = 13
   4,                                        // TR_ArrayCopyToc                        = 14
   4,                                        // TR_BodyInfoAddress                     = 15
   12,                                       // TR_Thunks                              = 16
   16,                                       // TR_StaticRamMethodConst                = 17
   12,                                       // TR_Trampolines                         = 18
   8,                                        // TR_PicTrampolines                      = 19
   8,                                        // TR_CheckMethodEnter                    = 20
   4,                                        // TR_RamMethod                           = 21
   8,                                        // TR_RamMethodSequence                   = 22
   8,                                        // TR_RamMethodSequenceReg                = 23
   24,                                       // TR_VerifyClassObjectForAlloc           = 24
   12,                                       // TR_ConstantPoolOrderedPair             = 25
   4,                                        // TR_AbsoluteMethodAddressOrderedPair    = 26
   20,                                       // TR_VerifyRefArrayForAlloc              = 27
   12,                                       // TR_J2IThunks                           = 28
   8,                                        // TR_GlobalValue                         = 29
   4,                                        // TR_BodyInfoAddressLoad                 = 30
   20,                                       // TR_ValidateInstanceField               = 31
   24,                                       // TR_InlinedStaticMethodWithNopGuard     = 32
   24,                                       // TR_InlinedSpecialMethodWithNopGuard    = 33
   24,                                       // TR_InlinedVirtualMethodWithNopGuard    = 34
   24,                                       // TR_InlinedInterfaceMethodWithNopGuard  = 35
   16,                                       // TR_SpecialRamMethodConst               = 36
   24,                                       // TR_InlinedHCRMethod                    = 37
   20,                                       // TR_ValidateStaticField                 = 38
   20,                                       // TR_ValidateClass                       = 39
   16,                                       // TR_ClassAddress                        = 40
   8,                                        // TR_HCR                                 = 41
   32,                                       // TR_ProfiledMethodGuardRelocation       = 42
   32,                                       // TR_ProfiledClassGuardRelocation        = 43
   0,                                        // TR_HierarchyGuardRelocation            = 44
   0,                                        // TR_AbstractGuardRelocation             = 45
   32,                                       // TR_ProfiledInlinedMethod               = 46
   20,                                       // TR_MethodPointer                       = 47
   16,                                       // TR_ClassPointer                        = 48
   8,                                        // TR_CheckMethodExit                     = 49
   12,                                       // TR_ValidateArbitraryClass              = 50
   0,                                        // TR_EmitClass(not used)                 = 51
   16,                                       // TR_JNISpecialTargetAddress             = 52
   16,                                       // TR_VirtualRamMethodConst               = 53
   20,                                       // TR_InlinedInterfaceMethod              = 54
   20,                                       // TR_InlinedVirtualMethod                = 55
   0,                                        // TR_NativeMethodAbsolute                = 56,
   0,                                        // TR_NativeMethodRelative                = 57,
   16,                                       // TR_ArbitraryClassAddress               = 58,
   28,                                        // TR_DebugCounter                        = 59
   4,                                        // TR_ClassUnloadAssumption               = 60
   16,                                       // TR_J2IVirtualThunkPointer              = 61
   };


#if 0

void J9::ARM::AheadOfTimeCompile::dumpRelocationData()
   {
   TR::Compilation *comp = TR::comp();
   uint8_t *cursor = getRelocationData();
   if (cursor)
      {
      uint8_t *endOfData = cursor + *(uint32_t *)cursor;
      diagnostic("Size of relocation data in AOT object is %d bytes\n"
                  "Size field in relocation data is         %d bytes\n", getSizeOfAOTRelocations(), *(uint32_t *)cursor);
      cursor += 4;

      while (cursor < endOfData)
         {
         diagnostic("\nSize of relocation is %d bytes.\n", *(uint16_t *)cursor);
         uint8_t *endOfCurrentRecord = cursor + *(uint16_t *)cursor;
         cursor += 2;
         TR_ExternalRelocationTargetKind kind = (TR_ExternalRelocationTargetKind)(*cursor & TR_ExternalRelocationTargetKindMask);
         diagnostic("Relocation type is %s.\n", TR::ExternalRelocation::getName(kind));
         int32_t offsetSize = (*cursor & RELOCATION_TYPE_WIDE_OFFSET) ? 4 : 2;
         bool isOrderedPair = (*cursor & RELOCATION_TYPE_ORDERED_PAIR)? true : false;
         diagnostic("Relocation uses %d-byte wide offsets.\n", offsetSize);
         diagnostic("Relocation uses %s offsets.\n", isOrderedPair ? "ordered-pair" : "non-paired");
         diagnostic("Relocation is %s.\n", (*cursor & RELOCATION_TYPE_EIP_OFFSET) ? "EIP Relative" : "Absolute");
         cursor++;
         diagnostic( "Target Info:\n");
         switch (kind)
            {
            case TR_ConstantPool:
               // constant pool address is placed as the last word of the header
               diagnostic("Constant pool %x", *(uint32_t *)++cursor);
               cursor += 4;
               break;
            case TR_HelperAddress:
               {
               // final byte of header is the index which indicates the particular helper being relocated to
               TR::SymbolReference *tempSR = comp->getSymRefTab()->getSymRef((int32_t)*cursor);
               diagnostic("Helper method address of %s(%d)", comp->getDebug()->getName(tempSR), (int32_t)*cursor++);
               }
               break;
            case TR_RelativeMethodAddress:
               // next word is the address of the constant pool to which the index refers
               // second word is the index in the above stored constant pool that indicates the particular
               // relocation target
               diagnostic("Relative Method Address: Constant pool = %x, index = %d", *(uint32_t *)(cursor+1), *(uint32_t *)(cursor+5));
               cursor += 9;
               break;
            case TR_AbsoluteMethodAddress:
               // Reference to the current method, no other information
               diagnostic("Absolute Method Address:");
               cursor++;
               break;
            case TR_DataAddress:
               // next word is the address of the constant pool to which the index refers
               // second word is the index in the above stored constant pool that indicates the particular
               // relocation target
               diagnostic("Data Address: Constant pool = %x, index = %d", *(uint32_t *)(cursor+1), *(uint32_t *)(cursor+5));
               cursor += 9;
               break;
            case TR_ClassObject:
               // next word is the address of the constant pool to which the index refers
               // second word is the index in the above stored constant pool that indicates the particular
               // relocation target
               diagnostic("Class Object: Constant pool = %x, index = %d", *(uint32_t *)(cursor+1), *(uint32_t *)(cursor+5));
               cursor += 9;
               break;
            case TR_MethodObject:
               // next word is the address of the constant pool to which the index refers
               // second word is the index in the above stored constant pool that indicates the particular
               // relocation target
               diagnostic("Method Object: Constant pool = %x, index = %d", *(uint32_t *)(cursor+1), *(uint32_t *)(cursor+5));
               cursor += 9;
               break;
            case TR_InterfaceObject:
               // next word is the address of the constant pool to which the index refers
               // second word is the index in the above stored constant pool that indicates the particular
               // relocation target
               diagnostic("Interface Object: Constant pool = %x, index = %d", *(uint32_t *)(cursor+1), *(uint32_t *)(cursor+5));
               cursor += 9;
               break;
            }
         diagnostic("\nUpdate location offsets:");
         uint8_t count = 0;
         if (offsetSize == 2)
            {
            while (cursor < endOfCurrentRecord)
               {
               if ((isOrderedPair && (count % 4)==0) ||
                   (!isOrderedPair && (count % 16)==0))
                  {
                  diagnostic("\n");
                  }
               count++;
               if (isOrderedPair)
		  {
                  diagnostic("(%04x ", *(uint16_t *)cursor);
                  cursor += 2;
                  diagnostic("%04x) ", *(uint16_t *)cursor);
                  cursor += 2;
		  }
               else
		  {
                  diagnostic("%04x ", *(uint16_t *)cursor);
                  cursor += 2;
		  }
               }
            }
         else
            {
            while (cursor < endOfCurrentRecord)
               {
               if ((isOrderedPair && (count % 2)==0) ||
                   (!isOrderedPair && (count % 8)==0))
                  {
                  diagnostic("\n");
                  }
               count++;
               if (isOrderedPair)
		  {
                  diagnostic("(%08x ", *(uint32_t *)cursor);
                  cursor += 4;
                  diagnostic("%08x) ", *(uint32_t *)cursor);
                  cursor += 4;
		  }
               else
		  {
                  diagnostic("%08x ", *(uint32_t *)cursor);
                  cursor += 4;
		  }
               }
            }
         diagnostic("\n");
         }

      diagnostic("\n\n");
      }
   else
      {
      diagnostic("No relocation data allocated\n");
      }
   }


#endif
