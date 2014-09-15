#include "stdafx.h"
#include "Utilities/Log.h"
#include "Emu/Cell/PPULLVMRecompiler.h"
#include "llvm/IR/Verifier.h"
#include "llvm/CodeGen/MachineCodeInfo.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define VERIFY_INSTRUCTION_AGAINST_INTERPRETER(fn, tc, input, ...) \
VerifyInstructionAgainstInterpreter(fmt::Format("%s.%d", #fn, tc).c_str(), &PPULLVMRecompilerWorker::fn, &PPUInterpreter::fn, input, __VA_ARGS__)

#define VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(fn, s, n, ...) {  \
    PPURegState input;                                                              \
    for (int i = s; i < (n + s); i++) {                                             \
        input.SetRandom();                                                          \
        VERIFY_INSTRUCTION_AGAINST_INTERPRETER(fn, i, input, __VA_ARGS__);          \
    }                                                                               \
}

/// Register state of a PPU
struct PPURegState {
    /// Floating point registers
    PPCdouble FPR[32];

    ///Floating point status and control register
    FPSCRhdr FPSCR;

    /// General purpose reggisters
    u64 GPR[32];

    /// Vector purpose registers
    VPR_reg VPR[32];

    /// Condition register
    CRhdr CR;

    /// Fixed point exception register
    XERhdr XER;

    /// Vector status and control register
    VSCRhdr VSCR;

    /// Link register
    u64 LR;

    /// Count register
    u64 CTR;

    /// SPR general purpose registers
    u64 SPRG[8];

    /// Time base register
    u64 TB;

    void Load(PPUThread & ppu) {
        for (int i = 0; i < 32; i++) {
            FPR[i] = ppu.FPR[i];
            GPR[i] = ppu.GPR[i];
            VPR[i] = ppu.VPR[i];

            if (i < 8) {
                SPRG[i] = ppu.SPRG[i];
            }
        }

        FPSCR = ppu.FPSCR;
        CR    = ppu.CR;
        XER   = ppu.XER;
        VSCR  = ppu.VSCR;
        LR    = ppu.LR;
        CTR   = ppu.CTR;
        TB    = ppu.TB;
    }

    void Store(PPUThread & ppu) {
        for (int i = 0; i < 32; i++) {
            ppu.FPR[i] = FPR[i];
            ppu.GPR[i] = GPR[i];
            ppu.VPR[i] = VPR[i];

            if (i < 8) {
                ppu.SPRG[i] = SPRG[i];
            }
        }

        ppu.FPSCR = FPSCR;
        ppu.CR    = CR;
        ppu.XER   = XER;
        ppu.VSCR  = VSCR;
        ppu.LR    = LR;
        ppu.CTR   = CTR;
        ppu.TB    = TB;
    }

    void SetRandom() {
        std::mt19937_64 rng;

        rng.seed(std::mt19937_64::default_seed);
        for (int i = 0; i < 32; i++) {
            FPR[i]         = (double)rng();
            GPR[i]         = rng();
            VPR[i]._u64[0] = rng();
            VPR[i]._u64[1] = rng();

            if (i < 8) {
                SPRG[i] = rng();
            }
        }

        FPSCR.FPSCR = (u32)rng();
        CR.CR       = (u32)rng();
        XER.XER     = 0;
        XER.CA      = (u32)rng();
        XER.SO      = (u32)rng();
        XER.OV      = (u32)rng();
        VSCR.VSCR   = (u32)rng();
        VSCR.X      = 0;
        VSCR.Y      = 0;
        LR          = rng();
        CTR         = rng();
        TB          = rng();
    }

    std::string ToString() const {
        std::string ret;

        for (int i = 0; i < 32; i++) {
            ret += fmt::Format("GPR[%02d] = 0x%016llx FPR[%02d] = %16g VPR[%02d] = 0x%s [%s]\n", i, GPR[i], i, FPR[i]._double, i, VPR[i].ToString(true).c_str(), VPR[i].ToString().c_str());
        }

        for (int i = 0; i < 8; i++) {
            ret += fmt::Format("SPRG[%d] = 0x%016llx\n", i, SPRG[i]);
        }

        ret += fmt::Format("CR      = 0x%08x LR = 0x%016llx CTR = 0x%016llx TB=0x%016llx\n", CR.CR, LR, CTR, TB);
        ret += fmt::Format("XER     = 0x%016llx [CA=%d | OV=%d | SO=%d]\n", XER.XER, fmt::by_value(XER.CA), fmt::by_value(XER.OV), fmt::by_value(XER.SO));
        ret += fmt::Format("FPSCR   = 0x%08x "
                           "[RN=%d | NI=%d | XE=%d | ZE=%d | UE=%d | OE=%d | VE=%d | "
                           "VXCVI=%d | VXSQRT=%d | VXSOFT=%d | FPRF=%d | "
                           "FI=%d | FR=%d | VXVC=%d | VXIMZ=%d | "
                           "VXZDZ=%d | VXIDI=%d | VXISI=%d | VXSNAN=%d | "
                           "XX=%d | ZX=%d | UX=%d | OX=%d | VX=%d | FEX=%d | FX=%d]\n",
                           FPSCR.FPSCR,
                           fmt::by_value(FPSCR.RN),
                           fmt::by_value(FPSCR.NI), fmt::by_value(FPSCR.XE), fmt::by_value(FPSCR.ZE), fmt::by_value(FPSCR.UE), fmt::by_value(FPSCR.OE), fmt::by_value(FPSCR.VE),
                           fmt::by_value(FPSCR.VXCVI), fmt::by_value(FPSCR.VXSQRT), fmt::by_value(FPSCR.VXSOFT), fmt::by_value(FPSCR.FPRF),
                           fmt::by_value(FPSCR.FI), fmt::by_value(FPSCR.FR), fmt::by_value(FPSCR.VXVC), fmt::by_value(FPSCR.VXIMZ),
                           fmt::by_value(FPSCR.VXZDZ), fmt::by_value(FPSCR.VXIDI), fmt::by_value(FPSCR.VXISI), fmt::by_value(FPSCR.VXSNAN),
                           fmt::by_value(FPSCR.XX), fmt::by_value(FPSCR.ZX), fmt::by_value(FPSCR.UX), fmt::by_value(FPSCR.OX), fmt::by_value(FPSCR.VX), fmt::by_value(FPSCR.FEX), fmt::by_value(FPSCR.FX));
        ret += fmt::Format("VSCR    = 0x%08x [NJ=%d | SAT=%d]\n", VSCR.VSCR, fmt::by_value(VSCR.NJ), fmt::by_value(VSCR.SAT));

        return ret;
    }
};

static PPUThread      * s_ppu_state    = nullptr;
static u64              s_base_address = 0;
static PPUInterpreter * s_interpreter  = nullptr;

template <class PPULLVMRecompilerFn, class PPUInterpreterFn, class... Args>
void PPULLVMRecompilerWorker::VerifyInstructionAgainstInterpreter(const char * name, PPULLVMRecompilerFn recomp_fn, PPUInterpreterFn interp_fn, PPURegState & input_reg_state, Args... args) {
    auto test_case = [&]() {
        (this->*recomp_fn)(args...);
    };
    auto input = [&]() {
        input_reg_state.Store(*s_ppu_state);
    };
    auto check_result = [&](std::string & msg) {
        PPURegState recomp_output_reg_state;
        PPURegState interp_output_reg_state;

        recomp_output_reg_state.Load(*s_ppu_state);
        input_reg_state.Store(*s_ppu_state);
        (s_interpreter->*interp_fn)(args...);
        interp_output_reg_state.Load(*s_ppu_state);

        if (interp_output_reg_state.ToString() != recomp_output_reg_state.ToString()) {
            msg = std::string("Input register states:\n") + input_reg_state.ToString() +
                  std::string("\nOutput register states:\n") + recomp_output_reg_state.ToString() +
                  std::string("\nInterpreter output register states:\n") + interp_output_reg_state.ToString();
            return false;
        }

        return true;
    };
    RunTest(name, test_case, input, check_result);
}

void PPULLVMRecompilerWorker::RunTest(const char * name, std::function<void()> test_case, std::function<void()> input, std::function<bool(std::string & msg)> check_result) {
    // Create the unit test function
    m_function = (Function *)m_module->getOrInsertFunction(name, m_ir_builder->getVoidTy(),
                                                           m_ir_builder->getInt8PtrTy() /*ppu_state*/,
                                                           m_ir_builder->getInt64Ty() /*base_addres*/,
                                                           m_ir_builder->getInt8PtrTy() /*interpreter*/, nullptr);
    m_function->setCallingConv(CallingConv::X86_64_Win64);
    auto arg_i = m_function->arg_begin();
    arg_i->setName("ppu_state");
    (++arg_i)->setName("base_address");
    (++arg_i)->setName("interpreter");

    auto block = BasicBlock::Create(*m_llvm_context, "start", m_function);
    m_ir_builder->SetInsertPoint(block);

    test_case();
    
    m_ir_builder->CreateRetVoid();

    // Print the IR
    std::string        ir;
    raw_string_ostream ir_ostream(ir);
    m_function->print(ir_ostream);
    LOG_NOTICE(PPU, "[UT %s] LLVM IR:%s", name, ir.c_str());

    std::string        verify;
    raw_string_ostream verify_ostream(verify);
    if (verifyFunction(*m_function, &verify_ostream)) {
        LOG_ERROR(PPU, "[UT %s] Verification Failed:%s", name, verify.c_str());
        return;
    }

    // Generate the function
    MachineCodeInfo mci;
    m_execution_engine->runJITOnFunction(m_function, &mci);

    // Disassember the generated function
    LOG_NOTICE(PPU, "[UT %s] Disassembly:", name);
    for (uint64_t pc = 0; pc < mci.size();) {
        char str[1024];

        auto size = LLVMDisasmInstruction(m_disassembler, (uint8_t *)mci.address() + pc, mci.size() - pc, (uint64_t)((uint8_t *)mci.address() + pc), str, sizeof(str));
        LOG_NOTICE(PPU, "[UT %s] %p: %s.", name, (uint8_t *)mci.address() + pc, str);
        pc += size;
    }

    // Run the test
    input();
    std::vector<GenericValue> args;
    args.push_back(GenericValue(s_ppu_state));
    GenericValue base_address;
    base_address.IntVal = APInt(64, s_base_address);
    args.push_back(base_address);
    args.push_back(GenericValue(s_interpreter));
    m_execution_engine->runFunction(m_function, args);

    // Verify results
    std::string msg;
    bool        pass = check_result(msg);
    if (pass) {
        LOG_NOTICE(PPU, "[UT %s] Test passed. %s", name, msg.c_str());
    } else {
        LOG_ERROR(PPU, "[UT %s] Test failed. %s", name, msg.c_str());
    }

    m_execution_engine->freeMachineCodeForFunction(m_function);
}

void PPULLVMRecompilerWorker::RunAllTests(PPUThread * ppu_state, u64 base_address, PPUInterpreter * interpreter) {
    s_ppu_state    = ppu_state;
    s_base_address = base_address;
    s_interpreter  = interpreter;

    PPURegState initial_state;
    initial_state.Load(*ppu_state);

    LOG_NOTICE(PPU, "Running Unit Tests");

    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MFVSCR, 0, 5, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTVSCR, 0, 5, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDCUW, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDFP, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDSBS, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDSHS, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDSWS, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDUBM, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDUBS, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDUHM, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDUHS, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDUWM, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDUWS, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VAND, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VANDC, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VAVGSB, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VAVGSH, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VAVGSW, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VAVGUB, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VAVGUH, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VAVGUW, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCFSX, 0, 5, 0, 0, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCFSX, 5, 5, 0, 3, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCFUX, 0, 5, 0, 0, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCFUX, 5, 5, 0, 2, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPBFP, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPBFP, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPBFP_, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPBFP_, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQFP, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQFP, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQFP_, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQFP_, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUB, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUB, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUB_, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUB_, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUH, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUH, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUH_, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUH_, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUW, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUW, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUW_, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUW_, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGEFP, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGEFP, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGEFP_, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGEFP_, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTFP, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTFP, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTFP_, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTFP_, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSB, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSB, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSB_, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSB_, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSH, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSH, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSH_, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSH_, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSW, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSW, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSW_, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSW_, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUB, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUB, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUB_, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUB_, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUH, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUH, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUH_, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUH_, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUW, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUW, 5, 5, 0, 1, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUW_, 0, 5, 0, 1, 2);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUW_, 5, 5, 0, 1, 1);
    // TODO: Rest of the vector instructions

    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MULLI, 0, 5, 1, 2, 12345);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SUBFIC, 0, 5, 1, 2, 12345);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CMPLI, 0, 5, 1, 0, 7, 12345);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CMPLI, 5, 5, 1, 1, 7, 12345);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CMPI, 0, 5, 5, 0, 7, -12345);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CMPI, 5, 5, 5, 1, 7, -12345);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDIC, 0, 5, 1, 2, 12345);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDIC_, 0, 5, 1, 2, 12345);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDI, 0, 5, 1, 2, 12345);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDI, 5, 5, 0, 2, 12345);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDIS, 0, 5, 1, 2, -12345);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDIS, 5, 5, 0, 2, -12345);
    // TODO: BC
    // TODO: SC
    // TODO: B
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MCRF, 0, 5, 0, 7);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MCRF, 5, 5, 6, 2);
    // TODO: BCLR
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CRNOR, 0, 5, 0, 7, 3);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CRANDC, 0, 5, 5, 6, 7);
    // TODO: ISYNC
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CRXOR, 0, 5, 7, 7, 7);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CRNAND, 0, 5, 3, 4, 5);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CRAND, 0, 5, 1, 2, 3);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CREQV, 0, 5, 2, 1, 0);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CRORC, 0, 5, 3, 4, 5);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CROR, 0, 5, 6, 7, 0);
    // TODO: BCCTR
    // TODO: RLWIMI
    // TODO: RLWINM
    // TODO: RLWNM
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ORI, 0, 5, 25, 29, 12345);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ORIS, 0, 5, 7, 31, -12345);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(XORI, 0, 5, 0, 19, 12345);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(XORIS, 0, 5, 3, 14, -12345);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ANDI_, 0, 5, 16, 7, 12345);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ANDIS_, 0, 5, 23, 21, -12345);

    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADD, 0, 5, 7, 8, 9, 0, 0);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADD, 5, 5, 21, 22, 23, 0, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(AND, 0, 5, 7, 8, 9, 0);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(AND, 5, 5, 21, 22, 23, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(OR, 0, 5, 7, 8, 9, 0);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(OR, 5, 5, 21, 22, 23, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(XOR, 0, 5, 7, 8, 9, 0);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(XOR, 5, 5, 21, 22, 23, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CMP, 0, 5, 3, 0, 9, 31);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CMP, 5, 5, 6, 1, 23, 14);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CMPL, 0, 5, 3, 0, 9, 31);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CMPL, 5, 5, 6, 1, 23, 14);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(EXTSB, 0, 5, 3, 5, 0);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(EXTSB, 5, 5, 3, 5, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(EXTSH, 0, 5, 6, 9, 0);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(EXTSH, 5, 5, 6, 9, 1);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(EXTSW, 0, 5, 25, 29, 0);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(EXTSW, 5, 5, 25, 29, 1);

    PPURegState input;
    input.SetRandom();
    input.GPR[14] = 10;
    input.GPR[23] = 0x10000;
    for (int i = 0; i < 1000; i++) {
        ((u8*)base_address)[i + 0x10000] = (u8)i;
    }

    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LBZ, 0, input, 5, 0, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LBZ, 1, input, 5, 14, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LBZU, 0, input, 5, 14, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LBZX, 0, input, 5, 0, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LBZX, 1, input, 5, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LBZUX, 0, input, 5, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHZ, 0, input, 5, 0, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHZ, 1, input, 5, 14, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHZU, 0, input, 5, 14, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHZX, 0, input, 5, 0, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHZX, 1, input, 5, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHZUX, 0, input, 5, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHA, 0, input, 5, 0, 0x100F0);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHA, 1, input, 5, 14, 0x100F0);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHAU, 0, input, 5, 14, 0x100F0);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHAX, 0, input, 5, 0, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHAX, 1, input, 5, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHAUX, 0, input, 5, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHBRX, 0, input, 5, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWZ, 0, input, 5, 0, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWZ, 1, input, 5, 14, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWZU, 0, input, 5, 14, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWZX, 0, input, 5, 0, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWZX, 1, input, 5, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWZUX, 0, input, 5, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWA, 0, input, 5, 0, 0x100F0);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWA, 1, input, 5, 14, 0x100F0);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWAX, 0, input, 5, 0, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWAX, 1, input, 5, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWAUX, 0, input, 5, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWBRX, 0, input, 5, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LD, 0, input, 5, 0, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LD, 1, input, 5, 14, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LDU, 0, input, 5, 14, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LDX, 0, input, 5, 0, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LDX, 1, input, 5, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LDUX, 0, input, 5, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LDBRX, 0, input, 5, 14, 23);

    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STB, 0, input, 3, 0, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STB, 1, input, 3, 14, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STBU, 0, input, 3, 14, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STBX, 0, input, 3, 0, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STBX, 1, input, 3, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STBUX, 0, input, 3, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STH, 0, input, 3, 0, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STH, 1, input, 3, 14, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STHU, 0, input, 3, 14, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STHX, 0, input, 3, 0, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STHX, 1, input, 3, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STHUX, 0, input, 3, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STHBRX, 0, input, 3, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STW, 0, input, 3, 0, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STW, 1, input, 3, 14, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STWU, 0, input, 3, 14, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STWX, 0, input, 3, 0, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STWX, 1, input, 3, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STWUX, 0, input, 3, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STWBRX, 0, input, 3, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STD, 0, input, 3, 0, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STD, 1, input, 3, 14, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STDU, 0, input, 3, 14, 0x10000);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STDX, 0, input, 3, 0, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STDX, 1, input, 3, 14, 23);
    VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STDUX, 0, input, 3, 14, 23);

    initial_state.Store(*ppu_state);
}
