#include "stdafx.h"

#include <iostream>     // TODO: Delete this file after removing all references to std::cout.

#include "ExpressionTree.h"
#include "ParameterNode.h"
#include "Node.h"
#include "X64CodeGenerator.h"


namespace NativeJIT
{
    //*************************************************************************
    //
    // RegisterFile
    //
    //*************************************************************************
    RegisterFile::RegisterFile(unsigned rxxCount, unsigned xmmCount)
        : m_rxxCount(rxxCount),
          m_xmmCount(xmmCount),
          m_reservedRXX((1u << rxxCount) - 1),
          m_reservedXMM((1u << xmmCount) - 1)
    {
    }


    unsigned RegisterFile::GetRXXRegisterCount() const
    {
        return m_rxxCount;
    }


    unsigned RegisterFile::GetXMMRegisterCount() const
    {
        return m_xmmCount;
    }


    unsigned RegisterFile::GetReservedRXX() const
    {
        return m_reservedRXX;
    }


    unsigned RegisterFile::GetReservedXMM() const
    {
        return m_reservedXMM;
    }


    //*************************************************************************
    //
    // ExpressionTree
    //
    //*************************************************************************
    ExpressionTree::ExpressionTree(Allocators::IAllocator& allocator, X64CodeGenerator& code)
        : m_allocator(allocator),
          m_code(code),
          m_parameterRegisters(code.GetRXXCount(), code.GetXMMCount()),
          m_basePointer(Register<sizeof(void*), false>(4)),     // TODO: Magic number RBP
          m_stackPointer(Register<sizeof(void*), false>(7)),    // TODO: Magic number RSP
          m_temporaryCount(0)
    {
    }


    Allocators::IAllocator& ExpressionTree::GetAllocator() const
    {
        return m_allocator;
    }


    unsigned ExpressionTree::AddNode(NodeBase& node)
    {
        m_topologicalSort.push_back(&node);
        return static_cast<unsigned>(m_topologicalSort.size() - 1);
    }


    unsigned ExpressionTree::AddParameter(ParameterBase& parameter)
    {
        m_parameters.push_back(&parameter);
        return static_cast<unsigned>(m_parameters.size() - 1);
    }


    RegisterFile& ExpressionTree::GetParameterRegisters()
    {
        return m_parameterRegisters;
    }


    X64CodeGenerator& ExpressionTree::GetCodeGenerator() const
    {
        return m_code;
    }


    Register<sizeof(void*), false> ExpressionTree::GetBasePointer() const
    {
        return m_basePointer;
    }


    size_t ExpressionTree::AllocateTemporary()
    {
        return m_temporaryCount++ * sizeof(void*);
    }


    void ExpressionTree::ReleaseTemporary(size_t offset)
    {
        // TODO: Implement.
//        return m_temporaryCount++ * sizeof(void*);
    }


    void ExpressionTree::SetBasePointer(Register<sizeof(void*), false> r)
    {
        m_basePointer = r;
    }


    void ExpressionTree::Print() const
    {
        std::cout << "Parameters:" << std::endl;
        for (unsigned i = 0 ; i < m_parameters.size(); ++i)
        {
            m_parameters[i]->PrintParameter();
            std::cout << std::endl;
        }
        std::cout << std::endl;

        std::cout << "Topological sort:" << std::endl;
        for (unsigned i = 0 ; i < m_topologicalSort.size(); ++i)
        {
            m_topologicalSort[i]->Print();
            std::cout << std::endl;
        }
        std::cout << std::endl;

        std::cout << "RXX Registers:" << std::endl;
        unsigned registers = 0;
        for (unsigned i = 0 ; i < m_rxxRegisters.size();  ++i)
        {
            registers |= (1 << m_rxxRegisters[i]);
        }
        for (unsigned i = 0 ; i < 16;  ++i)
        {
            std::cout << Register<8, false>(i).GetName();
            if ((registers & 1) == 0)
            {
                std::cout << " in use";
            }
            else
            {
                std::cout << " free";
            }
            registers >>= 1;
            std::cout << std::endl;
        }
        std::cout << std::endl;
    }


    void ExpressionTree::Compile()
    {
        Prologue();
        Pass1();
        Pass2();
        Print();
        Pass3();
        Epilogue();
//        Print();
    }


    void ExpressionTree::Prologue()
    {
        std::cout << "Prologue ..." << std::endl;
        std::cout << "  NOT IMPLEMENTED" << std::endl;

        m_code.Op("mov", m_basePointer, m_stackPointer);
    }


    void ExpressionTree::Pass1()
    {
//        std::cout << "=== Pass1 ===" << std::endl;

        // Reserve registers use to pass in parameters.
        for (unsigned i = 0 ; i < m_parameters.size(); ++i)
        {
            m_parameters[i]->ReserveRegister(*this, m_parameterRegisters);
        }


        // Populate m_rxxRegisters with those registers not used by parameters.
        unsigned registers = m_parameterRegisters.GetReservedRXX();
        for (unsigned i = 0; i < m_parameterRegisters.GetRXXRegisterCount(); ++i)
        {
            if ((registers & 1) == 1 && i != m_basePointer.GetId() && i != m_stackPointer.GetId())
            {
                m_rxxRegisters.push_back(i);
            }
            registers >>= 1;
        }


        // TODO: TEMPORARY CODE FOR DEBUGGING.
        for (unsigned i = 0; i < 16; ++i)
        {
            m_xmmRegisters.push_back(i);
        }
    }


    //
    // TODO: NOTE: With the addition of conditional nodes, the common
    // subexpression (CSE) code is now inefficient because some CSEs
    // may not need to be evaluated. Consider, for example
    //   (v == 1)? a : ((v == 2) ? a + b : b + c)
    // Depending on the value of v, either a or c may not need to be
    // evaluated. Should probably lazily evaluate CSEs as they are needed.
    //
    void ExpressionTree::Pass2()
    {
//        std::cout << "=== Pass2 ===" << std::endl;

        for (unsigned i = 0 ; i < m_topologicalSort.size(); ++i)
        {
            NodeBase& node = *m_topologicalSort[i];

            // Assert(parentCount of zero legal only for last node);

            if (node.GetParentCount() > 1 && !node.IsCached2())
            {
                node.CodeGenCache(*this);
            }
        }
    }


    void ExpressionTree::Pass3()
    {
//        std::cout << "=== Pass3 ===" << std::endl;

        NodeBase& root = *m_topologicalSort.back();

//        Assert(Root node is return, "Root must be a return node.");

        root.LabelSubtree(true);
        root.CompileAsRoot(*this);
    }


    void ExpressionTree::Epilogue()
    {
        std::cout << "Epilogue ..." << std::endl;
        std::cout << "  NOT IMPLEMENTED" << std::endl;
    }
}
