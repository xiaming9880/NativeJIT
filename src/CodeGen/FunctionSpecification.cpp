#include "stdafx.h"

#include <algorithm>    // For std::min.
#include <stdexcept>
#include <Windows.h>

#include "NativeJIT/BitOperations.h"
#include "NativeJIT/CallingConvention.h"
#include "NativeJIT/FunctionBuffer.h"
#include "NativeJIT/FunctionSpecification.h"
#include "Temporary/IAllocator.h"
#include "UnwindCode.h"


namespace NativeJIT
{
    //
    // FunctionSpecification
    //

    // Note: defining in .cpp file to avoid the need to include UnwindCode.h
    // in FunctionSpecification.h.
    const unsigned FunctionSpecification::c_maxUnwindInfoBufferSize
        = sizeof(UnwindInfo)
          - sizeof(UnwindCode) // Included in UnwindInfo.
          + c_maxUnwindCodes * sizeof(UnwindCode);

    FunctionSpecification::FunctionSpecification(Allocators::IAllocator& allocator,
                                                 int maxFunctionCallParameters,
                                                 unsigned localStackSlotCount,
                                                 unsigned savedRxxNonvolatilesMask,
                                                 unsigned savedXmmNonvolatilesMask,
                                                 BaseRegisterType baseRegisterType)
        : m_stlAllocator(allocator),
          m_unwindInfoBuffer(m_stlAllocator),
          m_prologCode(m_stlAllocator),
          m_epilogCode(m_stlAllocator)
    {
        // The code in this buffer will not be executed directly, so the general
        // allocator can be used for code buffer allocation as well.
        X64CodeGenerator code(allocator, c_maxPrologOrEpilogSize, allocator);

        BuildUnwindInfoAndProlog(maxFunctionCallParameters,
                                 localStackSlotCount,
                                 savedRxxNonvolatilesMask,
                                 savedXmmNonvolatilesMask,
                                 baseRegisterType,
                                 code,
                                 m_unwindInfoBuffer,
                                 m_offsetToOriginalRsp);

        m_prologCode.assign(code.BufferStart(),
                            code.BufferStart() + code.CurrentPosition());

        code.Reset();
        BuildEpilog(*reinterpret_cast<UnwindInfo*>(m_unwindInfoBuffer.data()),
                    code);

        m_epilogCode.assign(code.BufferStart(),
                            code.BufferStart() + code.CurrentPosition());
    }


    namespace
    {
        // Populates the current UnwindCode with the provided values and updates
        // the pointer to the predecessor unwind code (to facilitate reverse,
        // epilog-like order of unwind codes). Verifies that the current code does
        // not come before the starting boundary.
        void AddCodeAndBackDown(UnwindCode const * unwindCodesStart,
                                UnwindCode*& currUnwindCode,
                                unsigned codeOffset,
                                UnwindCodeOp op,
                                unsigned __int8 info)
        {
            Assert(currUnwindCode >= unwindCodesStart, "Unwind codes overflow");
            Assert(codeOffset <= (std::numeric_limits<unsigned __int8>::max)(),
                   "Code offset overflow: %u",
                   codeOffset);
            *currUnwindCode-- = UnwindCode(static_cast<unsigned __int8>(codeOffset),
                                           op,
                                           info);
        }


        // The flavor which populates unwind data that requires two unwind codes:
        // one with core information and the other one with 16-bit value.
        void AddCodeAndBackDown(UnwindCode const * unwindCodesStart,
                                UnwindCode*& currUnwindCode,
                                unsigned codeOffset,
                                UnwindCodeOp op,
                                unsigned __int8 info,
                                unsigned __int16 frameOffset)
        {
            // Since the codes are filled in reverse (epilog) order, place the
            // second operand first.
            Assert(currUnwindCode >= unwindCodesStart, "Unwind codes overflow");
            *currUnwindCode-- = UnwindCode(frameOffset);

            AddCodeAndBackDown(unwindCodesStart, currUnwindCode, codeOffset, op, info);
        }
    }


    void FunctionSpecification::BuildUnwindInfoAndProlog(int maxFunctionCallParameters,
                                                         unsigned localStackSlotCount,
                                                         unsigned savedRxxNonvolatilesMask,
                                                         unsigned savedXmmNonvolatilesMask,
                                                         BaseRegisterType baseRegisterType,
                                                         X64CodeGenerator& prologCode,
                                                         AllocatorVector<unsigned __int8>& unwindInfoBuffer,
                                                        __int32& offsetToOriginalRsp)
    {
        Assert((savedRxxNonvolatilesMask & ~CallingConvention::c_rxxWritableRegistersMask) == 0,
               "Saving/restoring of non-writable RXX registers is not allowed: 0x%Ix",
               savedRxxNonvolatilesMask & ~CallingConvention::c_rxxWritableRegistersMask);

        Assert((savedXmmNonvolatilesMask & ~CallingConvention::c_xmmWritableRegistersMask) == 0,
               "Saving/restoring of non-writable XMM registers is not allowed: 0x%Ix",
               savedXmmNonvolatilesMask & ~CallingConvention::c_xmmWritableRegistersMask);

        // Stack pointer is always saved/restored. However, unlike for the other
        // registers, it's done by subtracting/adding a value in the prolog/epilog.
        savedRxxNonvolatilesMask &= ~rsp.GetMask();

        // Ensure that the frame register is saved if used.
        if (baseRegisterType == BaseRegisterType::SetRbpToOriginalRsp)
        {
            savedRxxNonvolatilesMask |= rbp.GetMask();
        }

        const unsigned codeStartPos = prologCode.CurrentPosition();

        // If there are any function calls, at least 4 parameter slots need to
        // be allocated regardless of the actual parameter count.
        const unsigned functionParamsSlotCount
            = maxFunctionCallParameters >= 0
              ? (std::max)(maxFunctionCallParameters, 4)
              : 0;

        const unsigned rxxSavesCount = BitOp::GetNonZeroBitCount(savedRxxNonvolatilesMask);
        const unsigned xmmSavesCount = BitOp::GetNonZeroBitCount(savedXmmNonvolatilesMask);

        // All 128 bits of XMM registers need to saved in the prolog, so each
        // XMM register needs two slots. Also, XMM slots need to be 16-byte aligned,
        // so reserve one additional slot which may be needed for alignment.
        const unsigned regSavesSlotCount = rxxSavesCount
                                           + 2 * xmmSavesCount
                                           + (xmmSavesCount > 0 ? 1 : 0);

        // Calculate the total number of allocated stack slots. Ensure it's odd
        // since the stack pointer needs to be 16-byte aligned but it already
        // has one slot used for the return address.
        //
        // Note: there are some cases when it's not required to align the stack.
        // However, for simplicity and because the documentation is contradictory
        // (some sources say alignment is unnecessary only when there are no
        // function calls, some only when there is no stack allocation of any
        // type) the stack is always aligned here.
        //
        // Stack layout after setup:
        // [address 0] ---> [...]
        //     ---> [beginning of stack, 16-byte aligned; RSP adjusted by prolog points here]
        //         ---> [home space, empty or max(4, maxParametersInCallsByFunction); must be placed here]
        //         ---> [registers saved by prolog]
        //         ---> [local stack for temporaries etc]
        //     ---> [end of stack; original RSP pointed here; RBP points here if SetRbpToOriginalRsp]
        //     ---> [return address and parameters to the function]
        const unsigned totalStackSlotCount
            = (functionParamsSlotCount
               + regSavesSlotCount
               + localStackSlotCount)
              | 1;
        const unsigned totalStackBytes = totalStackSlotCount * sizeof(void*);
        offsetToOriginalRsp = totalStackBytes;

        Assert(totalStackBytes > 0 && totalStackBytes <= c_maxStackSize,
               "Invalid request for %u stack slots",
               totalStackSlotCount);

        // Need to use UWOP_ALLOC_SMALL for stack sizes from 8 to 128 bytes and
        // UWOP_ALLOC_LARGE otherwise. If using UWOP_ALLOC_LARGE, currently only
        // the version which uses two unwind codes is supported. That version
        // can allocate almost 512 kB, which is far more than the 4 kB limit
        // which would require a chkstk call.
        const bool isSmallStackAlloc = totalStackBytes <= 128;

        // Compute number of unwind codes needed. Each RXX/XMM save takes two
        // codes and stack allocation takes 1 for UWOP_ALLOC_SMALL and 2 for
        // UWOP_ALLOC_LARGE (also see the previous comment).
        const unsigned actualUnwindCodeCount
            = (rxxSavesCount + xmmSavesCount) * 2
              + (isSmallStackAlloc ? 1 : 2);

        Assert(actualUnwindCodeCount > 0
               && actualUnwindCodeCount <= c_maxUnwindCodes,
               "Invalid number of unwind codes: %u",
               actualUnwindCodeCount);

        // From MSDN UNWIND_INFO documentation for unwind codes array:
        // "For alignment purposes, this array will always have an even number
        // of entries, with the final entry potentially unused (in which case
        // the array will be one longer than indicated by the count of unwind
        // codes field)."
        const unsigned alignedCodeCount = (actualUnwindCodeCount + 1) & ~1u;

        // Allocate the memory. Account for the fact that one unwind code is
        // already included in UnwindInfo. Due to the check above that unwind
        // count is positive, the calculation will not overflow.
        unwindInfoBuffer.resize(sizeof(UnwindInfo)
                                + (alignedCodeCount - 1)
                                  * sizeof(UnwindCode));
        UnwindInfo* unwindInfo = reinterpret_cast<UnwindInfo*>(unwindInfoBuffer.data());

        // Initialize UnwindInfo.
        unwindInfo->m_countOfCodes = static_cast<unsigned char>(actualUnwindCodeCount);
        unwindInfo->m_version = 1;
        unwindInfo->m_flags = 0;
        unwindInfo->m_frameRegister = 0;
        unwindInfo->m_frameOffset = 0;

        // Unwind codes are placed to the array of codes in order that will be
        // used in epilog (i.e. reverse order of steps), so locate the end first.
        UnwindCode* unwindCodes = &unwindInfo->m_firstUnwindCode;
        UnwindCode* currUnwindCode = &unwindCodes[actualUnwindCodeCount - 1];

        // Start emitting the unwind codes and the opcodes for prolog. First,
        // adjust the stack pointer.
        prologCode.EmitImmediate<OpCode::Sub>(rsp, offsetToOriginalRsp);

        // Emit the matching unwind codes.
        if (isSmallStackAlloc)
        {
            Assert(totalStackSlotCount >= 1 && totalStackSlotCount <= 16,
                   "Logic error, alloc small slot count %u",
                   totalStackSlotCount);

            // The values 1-16 are encoded as 0-15, so subtract one.
            AddCodeAndBackDown(unwindCodes,
                               currUnwindCode,
                               prologCode.CurrentPosition() - codeStartPos,
                               UnwindCodeOp::UWOP_ALLOC_SMALL,
                               static_cast<unsigned __int8>(totalStackSlotCount - 1));
        }
        else
        {
            Assert(totalStackSlotCount >= 17
                   && totalStackSlotCount <= (std::numeric_limits<unsigned __int16>::max)(),
                   "Logic error, alloc large slot count %u",
                   totalStackSlotCount);

            // Value of 0 for info argument signifies two-code version of
            // UWOP_ALLOC_LARGE which is used for allocations from 136 to
            // 512 kB - 8 bytes (i.e. 17 to 65535 slots).
            AddCodeAndBackDown(unwindCodes,
                               currUnwindCode,
                               prologCode.CurrentPosition() - codeStartPos,
                               UnwindCodeOp::UWOP_ALLOC_LARGE,
                               static_cast<unsigned __int8>(0),
                               static_cast<unsigned __int16>(totalStackSlotCount));
        }

        // Save registers into the reserved area. The area comes right after
        // the initial slots reserved for parameters for function calls.
        unsigned currStackSlotOffset = functionParamsSlotCount;

        unsigned regId = 0;
        unsigned registersMask = savedRxxNonvolatilesMask;

        // Save the RXX registers.
        while (BitOp::GetLowestBitSet(registersMask, &regId))
        {
            prologCode.Emit<OpCode::Mov>(rsp,
                                         currStackSlotOffset * sizeof(void*),
                                         Register<8, false>(regId));

            AddCodeAndBackDown(unwindCodes,
                               currUnwindCode,
                               prologCode.CurrentPosition() - codeStartPos,
                               UnwindCodeOp::UWOP_SAVE_NONVOL,
                               static_cast<unsigned __int8>(regId),
                               static_cast<unsigned __int16>(currStackSlotOffset));

            BitOp::ClearBit(&registersMask, regId);
            currStackSlotOffset++;
        }

        // Save XMM registers.
        if (xmmSavesCount > 0)
        {
            // Ensure that slot offset is even (16-byte aligned). The additional
            // slot was already reserved for this case.
            if ((currStackSlotOffset & 1) != 0)
            {
                currStackSlotOffset++;
            }

            registersMask = savedXmmNonvolatilesMask;

            while (BitOp::GetLowestBitSet(registersMask, &regId))
            {
                // TODO: Implement movaps and use it to save all 128 bits (upcoming change).
                prologCode.Emit<OpCode::Mov>(rsp,
                                             currStackSlotOffset * sizeof(void*),
                                             Register<8, true>(regId));

                // The offset specifies 16-byte slots, thus the divide by two.
                // The offset was previously verified to be even.
                AddCodeAndBackDown(unwindCodes,
                                   currUnwindCode,
                                   prologCode.CurrentPosition() - codeStartPos,
                                   UnwindCodeOp::UWOP_SAVE_XMM128,
                                   static_cast<unsigned __int8>(regId),
                                   static_cast<unsigned __int16>(currStackSlotOffset / 2));

                BitOp::ClearBit(&registersMask, regId);
                currStackSlotOffset += 2;
            }
        }

        // Ensure that the unwind codes were filled exactly as planned by
        // comparing the last filled code with the beginning of the array. Since
        // currUnwindCode points to the place where the *next* code would have
        // been placed, currUnwindCode + 1 points to the last placed code.
        Assert(currUnwindCode + 1 == unwindCodes,
               "Mismatched count of unwind codes: %Id",
               currUnwindCode + 1 - unwindCodes);

        // Point the RBP to the original RSP value. Note: not using UWOP_SET_FPREG
        // since 1) it's not necessary on x64 as setting the base pointer is
        // only an optional convenience 2) The offset is limited to [0, 240] range,
        // which may not be enough and 3) some documentation sources state that
        // if used, UWOP_SET_FPREG must occur before any register saves that
        // specify offset, which complicates this function needlessly.
        if (baseRegisterType == BaseRegisterType::SetRbpToOriginalRsp)
        {
            // It's necessary to extend the last unwind code that recorded an
            // instruction offset into prolog to account for the instruction
            // to be added to set up RBP.
            Assert(unwindCodes[0].m_operation.m_codeOffset
                   == prologCode.CurrentPosition() - codeStartPos,
                   "Logical error in RBP adjustment, instruction offset %u vs %u",
                   unwindCodes[0].m_operation.m_codeOffset,
                   prologCode.CurrentPosition() - codeStartPos);

            prologCode.Emit<OpCode::Lea>(rbp, rsp, offsetToOriginalRsp);

            // Note: the cast is safe, the code buffer is length limited to
            // max size for prolog/epilog.
            unwindCodes[0].m_operation.m_codeOffset
                = static_cast<unsigned __int8>(prologCode.CurrentPosition() - codeStartPos);
        }

        // Code offset points to the next instruction after the end of prolog,
        // so (given that prolog starts from offset 0) it also specifies the
        // size of prolog.
        unwindInfo->m_sizeOfProlog = unwindCodes[0].m_operation.m_codeOffset;
    }


    // A helper function to return the number of unwind codes for an opcode.
    static unsigned GetUnwindOpCodeCount(UnwindCode code)
    {
        switch (static_cast<UnwindCodeOp>(code.m_operation.m_unwindOp))
        {
            case UnwindCodeOp::UWOP_ALLOC_SMALL:
            case UnwindCodeOp::UWOP_PUSH_MACHFRAME:
            case UnwindCodeOp::UWOP_PUSH_NONVOL:
            case UnwindCodeOp::UWOP_SET_FPREG:
                return 1;

            case UnwindCodeOp::UWOP_SAVE_NONVOL:
            case UnwindCodeOp::UWOP_SAVE_XMM128:
                return 2;

            case UnwindCodeOp::UWOP_SAVE_NONVOL_FAR:
            case UnwindCodeOp::UWOP_SAVE_XMM128_FAR:
                return 3;

            case UnwindCodeOp::UWOP_ALLOC_LARGE:
                Assert(code.m_operation.m_opInfo <= 1,
                       "Invalid OpInfo for UWOP_ALLOC_LARGE: %u",
                       code.m_operation.m_opInfo);

                return code.m_operation.m_opInfo == 0 ? 2 : 3;

            default:
                Assert(false, "Unknown unwind operation %u", code.m_operation.m_unwindOp);
                // Silence the "not all paths return a value" warning.
                return 0;
        }
    }


    void FunctionSpecification::BuildEpilog(UnwindInfo const & unwindInfo,
                                            X64CodeGenerator& epilogCode)
    {
        UnwindCode const * codes = &unwindInfo.m_firstUnwindCode;
        unsigned i = 0;

        while (i < unwindInfo.m_countOfCodes)
        {
            const UnwindCode unwindCode = codes[i];

            // Check how many codes the operation needs.
            const unsigned codeCount = GetUnwindOpCodeCount(unwindCode);
            Assert(i + codeCount <= unwindInfo.m_countOfCodes,
                   "Not enough unwind codes for op %u",
                   unwindCode.m_operation.m_unwindOp);

            // For 2 or more codes, read the data for the second code (always
            // the m_frameOffset union member).
            const unsigned code2Offset = codeCount >= 2
                                         ? codes[i + 1].m_frameOffset
                                         : 0;

            switch (static_cast<UnwindCodeOp>(unwindCode.m_operation.m_unwindOp))
            {
            case UnwindCodeOp::UWOP_ALLOC_LARGE:
                Assert(codeCount == 2, "Unexpected %u-code UWOP_ALLOC_LARGE", codeCount);
                // The second code contains the offset in quadwords.
                epilogCode.EmitImmediate<OpCode::Add>(rsp,
                                                      static_cast<__int32>(code2Offset
                                                                           * sizeof(void*)));
                break;

            case UnwindCodeOp::UWOP_ALLOC_SMALL:
                // The second code contains the slot count (in quadwords)
                // decreased by one.
                epilogCode.EmitImmediate<OpCode::Add>(
                    rsp,
                    static_cast<__int32>((unwindCode.m_operation.m_opInfo + 1)
                                         * sizeof(void*)));
                break;

            case UnwindCodeOp::UWOP_SAVE_NONVOL:
                // The second code contains the offset in quadwords.
                epilogCode.Emit<OpCode::Mov>(Register<8, false>(unwindCode.m_operation.m_opInfo),
                                             rsp,
                                             code2Offset * sizeof(void*));
                break;

            case UnwindCodeOp::UWOP_SAVE_XMM128:
                // The second code contains the halved offset in quadwords.
                // TODO: Implement movaps and use it to restore all 128 bits of register (upcoming change).
                epilogCode.Emit<OpCode::Mov>(Register<8, true>(unwindCode.m_operation.m_opInfo),
                                             rsp,
                                             code2Offset * 2 * sizeof(void*));
                break;

            default:
                Assert(false, "Unsupported unwind operation %u", unwindCode.m_operation.m_unwindOp);
                break;
            }

            i += codeCount;
        }

        // Return to caller.
        epilogCode.Emit<OpCode::Ret>();
    }


    __int32 FunctionSpecification::GetOffsetToOriginalRsp() const
    {
        return m_offsetToOriginalRsp;
    }


    unsigned __int8 const * FunctionSpecification::GetUnwindInfoBuffer() const
    {
        return m_unwindInfoBuffer.data();
    }


    unsigned FunctionSpecification::GetUnwindInfoByteLength() const
    {
        return static_cast<unsigned>(m_unwindInfoBuffer.size());
    }


    unsigned __int8 const * FunctionSpecification::GetProlog() const
    {
        return m_prologCode.data();
    }


    unsigned FunctionSpecification::GetPrologLength() const
    {
        return static_cast<unsigned>(m_prologCode.size());
    }

    
    unsigned __int8 const * FunctionSpecification::GetEpilog() const
    {
        return m_epilogCode.data();
    }
    
    
    unsigned FunctionSpecification::GetEpilogLength() const
    {
        return static_cast<unsigned>(m_epilogCode.size());
    }
}