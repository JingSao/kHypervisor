// Copyright (c) 2015-2016, Kelvin. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be
// found in the LICENSE file.

/// @file
/// Implements VMM functions.

#include "vmm.h"
#include <intrin.h>
#include "asm.h"
#include "common.h"
#include "ept.h"
#include "log.h"
#include "util.h"
#ifndef HYPERPLATFORM_PERFORMANCE_ENABLE_PERFCOUNTER
#define HYPERPLATFORM_PERFORMANCE_ENABLE_PERFCOUNTER 1
#endif  // HYPERPLATFORM_PERFORMANCE_ENABLE_PERFCOUNTER
#include "performance.h"
#include "../../DdiMon/shadow_hook.h"
#include "vmcs.h"


extern "C" {
	////////////////////////////////////////////////////////////////////////////////
	//
	// macro utilities
	// 

	// cpuid VMX features
#define MY_VMX_TPR_SHADOW            (1 <<  0)              /* TPR shadow */
#define MY_VMX_VIRTUAL_NMI           (1 <<  1)              /* Virtual NMI */
#define MY_VMX_APIC_VIRTUALIZATION   (1 <<  2)              /* APIC Access Virtualization */
#define MY_VMX_WBINVD_VMEXIT         (1 <<  3)              /* WBINVD VMEXIT */
#define MY_VMX_PERF_GLOBAL_CTRL      (1 <<  4)              /* Save/Restore MSR_PERF_GLOBAL_CTRL */
#define MY_VMX_MONITOR_TRAP_FLAG     (1 <<  5)              /* Monitor trap Flag (MTF) */
#define MY_VMX_X2APIC_VIRTUALIZATION (1 <<  6)              /* Virtualize X2APIC */
#define MY_VMX_EPT                   (1 <<  7)              /* Extended Page Tables (EPT) */
#define MY_VMX_VPID                  (1 <<  8)              /* VPID */
#define MY_VMX_UNRESTRICTED_GUEST    (1 <<  9)              /* Unrestricted Guest */
#define MY_VMX_PREEMPTION_TIMER      (1 << 10)              /* VMX preemption timer */
#define MY_VMX_SAVE_DEBUGCTL_DISABLE (1 << 11)              /* Disable Save/Restore of MSR_DEBUGCTL */
#define MY_VMX_PAT                   (1 << 12)              /* Save/Restore MSR_PAT */
#define MY_VMX_EFER                  (1 << 13)              /* Save/Restore MSR_EFER */
#define MY_VMX_DESCRIPTOR_TABLE_EXIT (1 << 14)              /* Descriptor Table VMEXIT */
#define MY_VMX_PAUSE_LOOP_EXITING    (1 << 15)              /* Pause Loop Exiting */
#define MY_VMX_EPTP_SWITCHING        (1 << 16)              /* EPTP switching (VM Function 0) */
#define MY_VMX_EPT_ACCESS_DIRTY      (1 << 17)              /* Extended Page Tables (EPT) A/D Bits */
#define MY_VMX_VINTR_DELIVERY        (1 << 18)              /* Virtual Interrupt Delivery */
#define MY_VMX_POSTED_INSTERRUPTS    (1 << 19)              /* Posted Interrupts support - not implemented yet */
#define MY_VMX_VMCS_SHADOWING        (1 << 20)              /* VMCS Shadowing */
#define MY_VMX_EPT_EXCEPTION         (1 << 21)              /* EPT Violation (#VE) exception */
#define MY_VMX_PML                   (1 << 22)              /* Page Modification Logging - not implemented yet */
#define MY_VMX_TSC_SCALING           (1 << 23)              /* TSC Scaling */

	////////////////////////////////////////////////////////////////////////////////
	//
	// constants and macros
	//

	// Whether VM-exit recording is enabled
	static const long kVmmpEnableRecordVmExit = false;

	// How many events should be recorded per a processor
	static const long kVmmpNumberOfRecords = 100;

	// How many processors are supported for recording
	static const long kVmmpNumberOfProcessors = 2;


	////////////////////////////////////////////////////////////////////////////////
	//
	// types
	//

	enum VMX_state
	{
		VMCS_STATE_CLEAR = 0,
		VMCS_STATE_LAUNCHED
	};


	typedef struct NestedVmm
	{
		ULONG64 vmxon_region;
		ULONG64 current_vmcs;				///VMCS02
		ULONG64 guest_vmcs;				///VMCS12 , i.e. L1 provided at the beginning
		ULONG   CpuNumber;				///vCPU number
		BOOLEAN blockINITsignal;			///NOT USED
		BOOLEAN blockAndDisableA20M;			///NOT USED
		BOOLEAN inVMX;					///is it in VMX mode 
		BOOLEAN inRoot;					///is it in root mode
		USHORT	kVirtualProcessorId;
	}NestedVmm, *PNestedVmm;


	// Represents raw structure of stack of VMM when VmmVmExitHandler() is called
	struct	VmmInitialStack
	{
		GpRegisters gp_regs;
		ULONG_PTR reserved;
		ProcessorData *processor_data;
	};

	// Things need to be read and written by each VM-exit handler
	struct GuestContext
	{
		union
		{
			VmmInitialStack *stack;
			GpRegisters *gp_regs;
		};
		FlagRegister flag_reg;
		ULONG_PTR ip;
		ULONG_PTR cr8;
		KIRQL irql;
		bool vm_continue;
	};
#if defined(_AMD64_)
	static_assert(sizeof(GuestContext) == 40, "Size check");
#else
	static_assert(sizeof(GuestContext) == 20, "Size check");
#endif

	// Context at the moment of vmexit
	struct VmExitHistory {
		GpRegisters gp_regs;
		ULONG_PTR ip;
		VmExitInformation exit_reason;
		ULONG_PTR exit_qualification;
		ULONG_PTR instruction_info;
	};

	////////////////////////////////////////////////////////////////////////////////
	//
	// prototypes
	//

	bool __stdcall VmmVmExitHandler(_Inout_ VmmInitialStack *stack);

	DECLSPEC_NORETURN void __stdcall VmmVmxFailureHandler(
		_Inout_ AllRegisters *all_regs);

	static void VmmpHandleVmExit(_Inout_ GuestContext *guest_context);

	DECLSPEC_NORETURN static void VmmpHandleTripleFault(
		_Inout_ GuestContext *guest_context);

	DECLSPEC_NORETURN static void VmmpHandleUnexpectedExit(
		_Inout_ GuestContext *guest_context);

	static void VmmpHandleMonitorTrap(_Inout_ GuestContext *guest_context);

	static void VmmpHandleException(_Inout_ GuestContext *guest_context);

	static void VmmpHandleCpuid(_Inout_ GuestContext *guest_context);

	static void VmmpHandleRdtsc(_Inout_ GuestContext *guest_context);

	static void VmmpHandleRdtscp(_Inout_ GuestContext *guest_context);

	static void VmmpHandleXsetbv(_Inout_ GuestContext *guest_context);

	static void VmmpHandleMsrReadAccess(_Inout_ GuestContext *guest_context);

	static void VmmpHandleMsrWriteAccess(_Inout_ GuestContext *guest_context);

	static void VmmpHandleMsrAccess(_Inout_ GuestContext *guest_context,
		_In_ bool read_access);

	static void VmmpHandleGdtrOrIdtrAccess(_Inout_ GuestContext *guest_context);

	static void VmmpHandleLdtrOrTrAccess(_Inout_ GuestContext *guest_context);

	static void VmmpHandleDrAccess(_Inout_ GuestContext *guest_context);

	static void VmmpHandleCrAccess(_Inout_ GuestContext *guest_context);

	static void VmmpHandleVmx(_Inout_ GuestContext *guest_context, VmxExitReason VMXReason);

	static void VmmpHandleVmCall(_Inout_ GuestContext *guest_context);

	static void VmmpHandleInvalidateInternalCaches(
		_Inout_ GuestContext *guest_context);

	static void VmmpHandleInvalidateTLBEntry(_Inout_ GuestContext *guest_context);

	static void VmmpHandleEptViolation(_Inout_ GuestContext *guest_context);

	static void VmmpHandleEptMisconfig(_Inout_ GuestContext *guest_context);

	static ULONG_PTR *VmmpSelectRegister(_In_ ULONG index,
		_In_ GuestContext *guest_context);

	static void VmmpDumpGuestSelectors();

	static void VmmpAdjustGuestInstructionPointer(_In_ ULONG_PTR guest_ip);

	typedef void(*pVmmpHandleVmExit)(GuestContext *guest_context);


	VOID VMSucceed(FlagRegister *reg);
	////////////////////////////////////////////////////////////////////////////////
	//
	// variables
	//

	// Those variables are all for diagnostic purpose
	static ULONG		 g_vmmp_next_history_index[kVmmpNumberOfProcessors];
	static VmExitHistory g_vmmp_vm_exit_history[kVmmpNumberOfProcessors][kVmmpNumberOfRecords];
	volatile LONG		 g_vpid = 1;
	volatile LONG	     g_VM_Core_Count = 0;
	NestedVmm*	         g_vcpus[64] = {};
	ULONG32				 g_vmx_extensions_bitmask;

	////////////////////////////////////////////////////////////////////////////////
	//
	// implementations
	//

	// A high level VMX handler called from AsmVmExitHandler().
	// Return true for vmresume, or return false for vmxoff.
#pragma warning(push)
#pragma warning(disable : 28167)
	_Use_decl_annotations_ bool __stdcall VmmVmExitHandler(VmmInitialStack *stack) {

		// Save guest's context and raise IRQL as quick as possible
		const auto guest_irql = KeGetCurrentIrql();
		const auto guest_cr8 = IsX64() ? __readcr8() : 0;

		if (guest_irql < DISPATCH_LEVEL)
		{
			KeRaiseIrqlToDpcLevel();
		}
		NT_ASSERT(stack->reserved == MAXULONG_PTR);

		// Capture the current guest state
		GuestContext guest_context = {
			stack,
			UtilVmRead(VmcsField::kGuestRflags),
			UtilVmRead(VmcsField::kGuestRip),
			guest_cr8,
			guest_irql,
			true
		};

		guest_context.gp_regs->sp = UtilVmRead(VmcsField::kGuestRsp);

		// Dispatch the current VM-exit event
		VmmpHandleVmExit(&guest_context);

		// Restore guest's context
		if (guest_context.irql < DISPATCH_LEVEL)
		{
			KeLowerIrql(guest_context.irql);
		}

		// Apply possibly updated CR8 by the handler
		if (IsX64())
		{
			__writecr8(guest_context.cr8);
		}
		return guest_context.vm_continue;
	}
#pragma warning(pop)
		BOOLEAN nested = FALSE;
	ULONG64 SearchVmcsByVpid(ULONG_PTR vpid)
	{
		int i = 0;
		for (i = 0; i < sizeof(g_vcpus) / sizeof(NestedVmm); i++)
		{
			if (g_vcpus[i]->kVirtualProcessorId == vpid)
			{
				break;
			}
		}
		return (ULONG64)UtilVaFromPa(g_vcpus[i]->guest_vmcs);  
	}
	VOID SaveExceptionInformationFromVmcs02(LONG_PTR vpid, VmExitInformation exit_reason)
	{
		int i = 0;
		for (i = 0; i < sizeof(g_vcpus) / sizeof(NestedVmm); i++)
		{
			if (g_vcpus[i]->kVirtualProcessorId == vpid)
			{
				//HYPERPLATFORM_LOG_DEBUG("Vpid Found: %x", vpid);
				break;
			}
		}

		//VMCS12
		ULONG64 guest_vmcs_va = (ULONG64)UtilVaFromPa(g_vcpus[i]->guest_vmcs);

		const VmExitInterruptionInformationField exception = {
			static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitIntrInfo))
		};

		ULONG_PTR vmexit_qualification = UtilVmRead(VmcsField::kExitQualification); 
		
		VmWrite32(VmcsField::kVmExitIntrInfo, guest_vmcs_va, exception.all);
		VmWrite32(VmcsField::kVmExitReason, guest_vmcs_va, exit_reason.all);
		VmWrite32(VmcsField::kExitQualification, guest_vmcs_va, vmexit_qualification);
		VmWrite32(VmcsField::kVmExitInstructionLen, guest_vmcs_va, UtilVmRead(VmcsField::kVmExitInstructionLen));
		VmWrite32(VmcsField::kVmInstructionError, guest_vmcs_va, UtilVmRead(VmcsField::kVmInstructionError));
		VmWrite32(VmcsField::kVmExitIntrErrorCode, guest_vmcs_va, UtilVmRead(VmcsField::kVmExitIntrErrorCode));
		VmWrite32(VmcsField::kIdtVectoringInfoField, guest_vmcs_va, UtilVmRead(VmcsField::kIdtVectoringInfoField));
		VmWrite32(VmcsField::kIdtVectoringErrorCode, guest_vmcs_va, UtilVmRead(VmcsField::kIdtVectoringErrorCode));
		VmWrite32(VmcsField::kVmxInstructionInfo, guest_vmcs_va, UtilVmRead(VmcsField::kVmxInstructionInfo));
	}


	VOID SaveGuestFieldFromVmcs02(ULONG_PTR vpid)
	{
		ULONG64 guest_vmcs_va = SearchVmcsByVpid(vpid);
		//all nested vm-exit should record 
		VmWrite64(VmcsField::kGuestRip, guest_vmcs_va, UtilVmRead(VmcsField::kGuestRip));
		VmWrite64(VmcsField::kGuestRsp, guest_vmcs_va, UtilVmRead(VmcsField::kGuestRsp));
		VmWrite64(VmcsField::kGuestCr3, guest_vmcs_va, UtilVmRead(VmcsField::kGuestCr3));
		VmWrite64(VmcsField::kGuestCr0, guest_vmcs_va, UtilVmRead(VmcsField::kGuestCr0));
		VmWrite64(VmcsField::kGuestCr4, guest_vmcs_va, UtilVmRead(VmcsField::kGuestCr4));
		VmWrite64(VmcsField::kGuestDr7, guest_vmcs_va, UtilVmRead(VmcsField::kGuestDr7)); 
		VmWrite64(VmcsField::kGuestRflags, guest_vmcs_va, UtilVmRead(VmcsField::kGuestRflags));


		VmWrite16(VmcsField::kGuestEsSelector,   guest_vmcs_va, UtilVmRead(VmcsField::kGuestEsSelector));
		VmWrite16(VmcsField::kGuestCsSelector,   guest_vmcs_va, UtilVmRead(VmcsField::kGuestCsSelector));
		VmWrite16(VmcsField::kGuestSsSelector,   guest_vmcs_va, UtilVmRead(VmcsField::kGuestSsSelector));
		VmWrite16(VmcsField::kGuestDsSelector,   guest_vmcs_va, UtilVmRead(VmcsField::kGuestDsSelector));
		VmWrite16(VmcsField::kGuestFsSelector,   guest_vmcs_va, UtilVmRead(VmcsField::kGuestFsSelector));
		VmWrite16(VmcsField::kGuestGsSelector,   guest_vmcs_va, UtilVmRead(VmcsField::kGuestGsSelector));
		VmWrite16(VmcsField::kGuestLdtrSelector, guest_vmcs_va, UtilVmRead(VmcsField::kGuestLdtrSelector));
		VmWrite16(VmcsField::kGuestTrSelector,   guest_vmcs_va, UtilVmRead(VmcsField::kGuestTrSelector));

		VmWrite32(VmcsField::kGuestEsLimit, guest_vmcs_va, UtilVmRead(VmcsField::kGuestEsLimit));
		VmWrite32(VmcsField::kGuestCsLimit, guest_vmcs_va, UtilVmRead(VmcsField::kGuestCsLimit));
		VmWrite32(VmcsField::kGuestSsLimit, guest_vmcs_va, UtilVmRead(VmcsField::kGuestSsLimit));
		VmWrite32(VmcsField::kGuestDsLimit, guest_vmcs_va, UtilVmRead(VmcsField::kGuestDsLimit));
		VmWrite32(VmcsField::kGuestFsLimit, guest_vmcs_va, UtilVmRead(VmcsField::kGuestFsLimit));
		VmWrite32(VmcsField::kGuestGsLimit, guest_vmcs_va,   UtilVmRead(VmcsField::kGuestGsLimit));
		VmWrite32(VmcsField::kGuestLdtrLimit, guest_vmcs_va, UtilVmRead(VmcsField::kGuestLdtrLimit));
		VmWrite32(VmcsField::kGuestTrLimit,   guest_vmcs_va, UtilVmRead(VmcsField::kGuestTrLimit));
		VmWrite32(VmcsField::kGuestGdtrLimit, guest_vmcs_va, UtilVmRead(VmcsField::kGuestGdtrLimit));
		VmWrite32(VmcsField::kGuestIdtrLimit, guest_vmcs_va, UtilVmRead(VmcsField::kGuestIdtrLimit));
		VmWrite32(VmcsField::kGuestEsArBytes, guest_vmcs_va, UtilVmRead(VmcsField::kGuestEsArBytes));
		VmWrite32(VmcsField::kGuestCsArBytes, guest_vmcs_va, UtilVmRead(VmcsField::kGuestCsArBytes));
		VmWrite32(VmcsField::kGuestSsArBytes, guest_vmcs_va, UtilVmRead(VmcsField::kGuestSsArBytes));
		VmWrite32(VmcsField::kGuestDsArBytes, guest_vmcs_va, UtilVmRead(VmcsField::kGuestDsArBytes));
		VmWrite32(VmcsField::kGuestFsArBytes, guest_vmcs_va, UtilVmRead(VmcsField::kGuestFsArBytes));
		VmWrite32(VmcsField::kGuestGsArBytes, guest_vmcs_va, UtilVmRead(VmcsField::kGuestGsArBytes));
		VmWrite32(VmcsField::kGuestLdtrArBytes, guest_vmcs_va, UtilVmRead(VmcsField::kGuestLdtrArBytes));
		VmWrite32(VmcsField::kGuestTrArBytes, guest_vmcs_va, UtilVmRead(VmcsField::kGuestTrArBytes));
		VmWrite32(VmcsField::kGuestInterruptibilityInfo, guest_vmcs_va, UtilVmRead(VmcsField::kGuestInterruptibilityInfo));
		VmWrite32(VmcsField::kGuestActivityState, guest_vmcs_va, UtilVmRead(VmcsField::kGuestActivityState));
		VmWrite32(VmcsField::kGuestSysenterCs, guest_vmcs_va, UtilVmRead(VmcsField::kGuestSysenterCs));  

		VmWrite64(VmcsField::kGuestSysenterEsp, guest_vmcs_va, UtilVmRead(VmcsField::kGuestSysenterEsp));
		VmWrite64(VmcsField::kGuestSysenterEip, guest_vmcs_va, UtilVmRead(VmcsField::kGuestSysenterEip));
		VmWrite64(VmcsField::kGuestPendingDbgExceptions, guest_vmcs_va, UtilVmRead(VmcsField::kGuestPendingDbgExceptions));
		VmWrite64(VmcsField::kGuestEsBase, guest_vmcs_va, UtilVmRead(VmcsField::kGuestEsBase));
		VmWrite64(VmcsField::kGuestCsBase, guest_vmcs_va, UtilVmRead(VmcsField::kGuestCsBase));
		VmWrite64(VmcsField::kGuestSsBase, guest_vmcs_va, UtilVmRead(VmcsField::kGuestSsBase));
		VmWrite64(VmcsField::kGuestDsBase, guest_vmcs_va, UtilVmRead(VmcsField::kGuestDsBase));
		VmWrite64(VmcsField::kGuestFsBase, guest_vmcs_va, UtilVmRead(VmcsField::kGuestFsBase));
		VmWrite64(VmcsField::kGuestGsBase, guest_vmcs_va, UtilVmRead(VmcsField::kGuestGsBase));
		VmWrite64(VmcsField::kGuestLdtrBase, guest_vmcs_va, UtilVmRead(VmcsField::kGuestLdtrBase));
		VmWrite64(VmcsField::kGuestTrBase, guest_vmcs_va, UtilVmRead(VmcsField::kGuestTrBase));
		VmWrite64(VmcsField::kGuestGdtrBase, guest_vmcs_va, UtilVmRead(VmcsField::kGuestGdtrBase));
		VmWrite64(VmcsField::kGuestIdtrBase, guest_vmcs_va, UtilVmRead(VmcsField::kGuestIdtrBase)); 

		//Read - only field
		//Write to VMCS12 for vmread
		const VmExitInformation exit_reason = { static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitReason)) };
		const VmExitInterruptionInformationField exception = {static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitIntrInfo))};

		SaveExceptionInformationFromVmcs02(vpid, exit_reason);

	}
	
	VOID EmulateVmExit(ULONG_PTR vpid, ULONG64 vmcs01)
	{

		VmxStatus status; 
		ULONG64   vmcs12			  = SearchVmcsByVpid(vpid);
		ULONG64   VMCS_VMEXIT_HANDLER = 0;
		ULONG64   VMCS_VMEXIT_STACK   = 0; 
		ULONG64   VMCS_VMEXIT_RFLAGs = 0;
		ULONG64   VMCS_VMEXIT_CR4 = 0;
		ULONG64   VMCS_VMEXIT_CR3 = 0;
		ULONG64   VMCS_VMEXIT_CR0 = 0;


		ULONG64   VMCS_VMEXIT_CS = 0;
		ULONG64   VMCS_VMEXIT_SS = 0;
		ULONG64   VMCS_VMEXIT_DS = 0;
		ULONG64   VMCS_VMEXIT_ES = 0;
		ULONG64   VMCS_VMEXIT_FS = 0;
		ULONG64   VMCS_VMEXIT_GS = 0;
		ULONG64   VMCS_VMEXIT_TR = 0;

		ULONG32   VMCS_VMEXIT_SYSENTER_CS = 0;
		ULONG64   VMCS_VMEXIT_SYSENTER_RIP = 0;
		ULONG64   VMCS_VMEXIT_SYSENTER_RSP = 0;


		ULONG_PTR  VMCS_VMEXIT_HOST_FS = 0;
		ULONG_PTR  VMCS_VMEXIT_HOST_GS = 0;
		ULONG_PTR  VMCS_VMEXIT_HOST_TR = 0;

		//VMCS01 guest rip == VMCS12 host rip (should be)
		const VmExitInformation exit_reason = { static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitReason)) };

		const VmExitInterruptionInformationField exception = {
			static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitIntrInfo))
		};

		PrintVMCS();

		HYPERPLATFORM_LOG_DEBUG("[EmulateVmExit]VMCS id %x", UtilVmRead(VmcsField::kVirtualProcessorId));
		HYPERPLATFORM_LOG_DEBUG("[EmulateVmExit]Trapped by %I64X ", UtilVmRead(VmcsField::kGuestRip));
		HYPERPLATFORM_LOG_DEBUG("[EmulateVmExit]Trapped Reason: %I64X ", exit_reason.fields.reason);
		HYPERPLATFORM_LOG_DEBUG("[EmulateVmExit]Trapped Intrreupt: %I64X ", exception.fields.interruption_type);
		HYPERPLATFORM_LOG_DEBUG("[EmulateVmExit]Trapped Intrreupt vector: %I64X ", exception.fields.vector);
		HYPERPLATFORM_LOG_DEBUG("[EmulateVmExit]Trapped kVmExitInstructionLen: %I64X ", UtilVmRead(VmcsField::kVmExitInstructionLen));

		if (VmxStatus::kOk != (status = static_cast<VmxStatus>(__vmx_vmptrld(&vmcs01))))
		{
			VmxInstructionError error = static_cast<VmxInstructionError>(UtilVmRead(VmcsField::kVmInstructionError));
			HYPERPLATFORM_LOG_DEBUG("Error vmptrld error code :%x , %x", status, error); 
		}

		//Read from VMCS12 get it host vmexit handler
		VmRead64(VmcsField::kHostRip, vmcs12, &VMCS_VMEXIT_HANDLER);
		VmRead64(VmcsField::kHostRsp, vmcs12, &VMCS_VMEXIT_STACK);
		VmRead64(VmcsField::kHostCr0, vmcs12, &VMCS_VMEXIT_CR0);
		VmRead64(VmcsField::kHostCr3, vmcs12, &VMCS_VMEXIT_CR3);
		VmRead64(VmcsField::kHostCr4, vmcs12, &VMCS_VMEXIT_CR4);


		VmRead64(VmcsField::kHostCsSelector, vmcs12, &VMCS_VMEXIT_CS);
		VmRead64(VmcsField::kHostSsSelector, vmcs12, &VMCS_VMEXIT_SS);
		VmRead64(VmcsField::kHostDsSelector, vmcs12, &VMCS_VMEXIT_DS);
		VmRead64(VmcsField::kHostEsSelector, vmcs12, &VMCS_VMEXIT_ES);
		VmRead64(VmcsField::kHostFsSelector, vmcs12, &VMCS_VMEXIT_FS);
		VmRead64(VmcsField::kHostGsSelector, vmcs12, &VMCS_VMEXIT_GS);
		VmRead64(VmcsField::kHostTrSelector, vmcs12, &VMCS_VMEXIT_TR);


		VmRead32(VmcsField::kHostIa32SysenterCs,  vmcs12, &VMCS_VMEXIT_SYSENTER_CS);
		VmRead64(VmcsField::kHostIa32SysenterEip, vmcs12, &VMCS_VMEXIT_SYSENTER_RSP);
		VmRead64(VmcsField::kHostIa32SysenterEsp, vmcs12, &VMCS_VMEXIT_SYSENTER_RIP);

		
		VmRead64(VmcsField::kHostFsBase, vmcs12, &VMCS_VMEXIT_HOST_FS);
		VmRead64(VmcsField::kHostGsBase, vmcs12, &VMCS_VMEXIT_HOST_GS);
		VmRead64(VmcsField::kHostTrBase, vmcs12, &VMCS_VMEXIT_HOST_TR);

		VmRead64(VmcsField::kGuestRflags,vmcs12, &VMCS_VMEXIT_RFLAGs);

		//Write VMCS01 for L1's VMExit handler
		UtilVmWrite(VmcsField::kGuestRflags, VMCS_VMEXIT_RFLAGs);
		UtilVmWrite(VmcsField::kGuestRip, VMCS_VMEXIT_HANDLER);
		UtilVmWrite(VmcsField::kGuestRsp, VMCS_VMEXIT_STACK);	
		UtilVmWrite(VmcsField::kGuestCr0, VMCS_VMEXIT_CR0);
		UtilVmWrite(VmcsField::kGuestCr3, VMCS_VMEXIT_CR3);
		UtilVmWrite(VmcsField::kGuestCr4, VMCS_VMEXIT_CR4);
		UtilVmWrite(VmcsField::kGuestDr7, 0x400);

		UtilVmWrite(VmcsField::kGuestCsSelector, VMCS_VMEXIT_CS);
		UtilVmWrite(VmcsField::kGuestSsSelector, VMCS_VMEXIT_SS);
		UtilVmWrite(VmcsField::kGuestDsSelector, VMCS_VMEXIT_DS);
		UtilVmWrite(VmcsField::kGuestEsSelector, VMCS_VMEXIT_ES);
		UtilVmWrite(VmcsField::kGuestFsSelector, VMCS_VMEXIT_FS);
		UtilVmWrite(VmcsField::kGuestGsSelector, VMCS_VMEXIT_GS);

		UtilVmWrite(VmcsField::kGuestSysenterCs,  VMCS_VMEXIT_SYSENTER_CS);
		UtilVmWrite(VmcsField::kGuestSysenterEsp, VMCS_VMEXIT_SYSENTER_RSP);
		UtilVmWrite(VmcsField::kGuestSysenterEip, VMCS_VMEXIT_SYSENTER_RIP);
		
		UtilVmWrite(VmcsField::kGuestFsBase, VMCS_VMEXIT_HOST_FS);
		UtilVmWrite(VmcsField::kGuestGsBase, VMCS_VMEXIT_HOST_GS);
		UtilVmWrite(VmcsField::kGuestTrBase, VMCS_VMEXIT_HOST_TR);
		
		PrintVMCS();
		 
		HYPERPLATFORM_LOG_DEBUG("[EmulateVmExit]VMCS01: kGuestRip :%I64x , kGuestRsp %I64x ", UtilVmRead(VmcsField::kGuestRip), UtilVmRead(VmcsField::kGuestRsp));
		HYPERPLATFORM_LOG_DEBUG("[EmulateVmExit]VMCS01: kHostRip :%I64x , kHostRsp %I64x ", UtilVmRead(VmcsField::kHostRip), UtilVmRead(VmcsField::kHostRsp)); 
	 
	}
	//Nested breakpoint dispatcher
	VOID NestedBreakpointHandler(GuestContext* guest_context)
	{
		const VmExitInformation exit_reason = { static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitReason)) };
		ULONG_PTR vpid = UtilVmRead(VmcsField::kVirtualProcessorId);

		//Trapped by VMCS02 - L2, and we redirect a interrupt information to L1 vm exit handler
		//TODO: We should check about if L2 want received this information
		if (vpid)
		{ 
		 	ULONG64   vmcs01 = UtilPaFromVa((void*)guest_context->stack->processor_data->vmcs_region);
 			EmulateVmExit(vpid, vmcs01);  
			return;
		}

		//Trapped by VMCS01 / L1 , normally handle it
		if (!vpid)
		{
			const VmExitInterruptionInformationField exception = {
				static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitIntrInfo))
			};

			HYPERPLATFORM_LOG_DEBUG("VMCS id %x", UtilVmRead(VmcsField::kVirtualProcessorId));
			HYPERPLATFORM_LOG_DEBUG("Trapped by %I64X ", UtilVmRead(VmcsField::kGuestRip));
			HYPERPLATFORM_LOG_DEBUG("Trapped Reason: %I64X ", exit_reason.fields.reason);
			HYPERPLATFORM_LOG_DEBUG("Trapped Intrreupt: %I64X ", exception.fields.interruption_type);
			HYPERPLATFORM_LOG_DEBUG("Trapped Intrreupt vector: %I64X ", exception.fields.vector);
			HYPERPLATFORM_LOG_DEBUG("Trapped kVmExitInstructionLen: %I64X ", UtilVmRead(VmcsField::kVmExitInstructionLen));

			VMSucceed(&guest_context->flag_reg);
			UtilVmWrite(VmcsField::kGuestRip, UtilVmRead(VmcsField::kGuestRip) + UtilVmRead(VmcsField::kVmExitInstructionLen));
			UtilVmWrite(VmcsField::kGuestRflags, guest_context->flag_reg.all); 
			return;
		}
	}
	// Dispatches VM-exit to a corresponding handler
	_Use_decl_annotations_ static void VmmpHandleVmExit(GuestContext *guest_context) {
		//HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		const VmExitInformation exit_reason = { static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitReason)) };
		
		ULONG_PTR vpid = UtilVmRead(VmcsField::kVirtualProcessorId);
		
		//Trapped by VMCS02 - L2, and we redirect a interrupt information to L1 vm exit handler
		//TODO: We should check about if L2 want received this information
		if (vpid)
		{
			SaveGuestFieldFromVmcs02(vpid);
		}
		
		if (kVmmpEnableRecordVmExit)
		{
			// Save them for ease of trouble shooting
			const auto processor = KeGetCurrentProcessorNumberEx(nullptr);
			auto &index = g_vmmp_next_history_index[processor];
			auto &history = g_vmmp_vm_exit_history[processor][index];

			history.gp_regs = *guest_context->gp_regs;
			history.ip = guest_context->ip;
			history.exit_reason = exit_reason;
			history.exit_qualification = UtilVmRead(VmcsField::kExitQualification);
			history.instruction_info = UtilVmRead(VmcsField::kVmxInstructionInfo);
			if (++index == kVmmpNumberOfRecords)
			{
				index = 0;
			}
		}

		switch (exit_reason.fields.reason) {
		case VmxExitReason::kExceptionOrNmi:
		{
			const VmExitInterruptionInformationField exception =
			{
				static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitIntrInfo))
			};

			if (static_cast<InterruptionVector>(exception.fields.vector) == InterruptionVector::kBreakpointException)
			{
				NestedBreakpointHandler(guest_context);
				break;
			}
			else
			{
				VmmpHandleException(guest_context);
			}
		}
		break;
		case VmxExitReason::kTripleFault:
			VmmpHandleTripleFault(guest_context);
			break;
		case VmxExitReason::kCpuid:
			VmmpHandleCpuid(guest_context);
			break;
		case VmxExitReason::kInvd:
			VmmpHandleInvalidateInternalCaches(guest_context);
			break;
		case VmxExitReason::kInvlpg:
			VmmpHandleInvalidateTLBEntry(guest_context);
			break;
		case VmxExitReason::kRdtsc:
			VmmpHandleRdtsc(guest_context);
			break;
		case VmxExitReason::kCrAccess:
			VmmpHandleCrAccess(guest_context);
			break;
		case VmxExitReason::kDrAccess:
			VmmpHandleDrAccess(guest_context);
			break;
		case VmxExitReason::kMsrRead:
			VmmpHandleMsrReadAccess(guest_context);
			break;
		case VmxExitReason::kMsrWrite:
			VmmpHandleMsrWriteAccess(guest_context);
			break;
		case VmxExitReason::kMonitorTrapFlag:
			VmmpHandleMonitorTrap(guest_context);
			break;
		case VmxExitReason::kGdtrOrIdtrAccess:
			VmmpHandleGdtrOrIdtrAccess(guest_context);
			break;
		case VmxExitReason::kLdtrOrTrAccess:
			VmmpHandleLdtrOrTrAccess(guest_context);
			break;
		case VmxExitReason::kEptViolation:
			VmmpHandleEptViolation(guest_context);
			break;
		case VmxExitReason::kEptMisconfig:
			VmmpHandleEptMisconfig(guest_context);
			break;
		case VmxExitReason::kVmcall:
			VmmpHandleVmCall(guest_context);
			break;
		case VmxExitReason::kVmclear:
		case VmxExitReason::kVmlaunch:
		case VmxExitReason::kVmptrld:
		case VmxExitReason::kVmptrst:
		case VmxExitReason::kVmread:
		case VmxExitReason::kVmresume:
		case VmxExitReason::kVmwrite:
		case VmxExitReason::kVmoff:
		case VmxExitReason::kVmon:
		case VmxExitReason::kVmxPreemptionTime:
		case VmxExitReason::kInvept:
			VmmpHandleVmx(guest_context, exit_reason.fields.reason);
			break;
		case VmxExitReason::kRdtscp:
			VmmpHandleRdtscp(guest_context);
			break;
		case VmxExitReason::kXsetbv:
			VmmpHandleXsetbv(guest_context);
			break;
		default:
			VmmpHandleUnexpectedExit(guest_context);
			break;
		}
	}

	// Triple fault VM-exit. Fatal error.
	_Use_decl_annotations_ static void VmmpHandleTripleFault(
		GuestContext *guest_context) {
		VmmpDumpGuestSelectors();
		HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kTripleFaultVmExit,
			reinterpret_cast<ULONG_PTR>(guest_context),
			guest_context->ip, 0);
	}

	// Unexpected VM-exit. Fatal error.
	_Use_decl_annotations_ static void VmmpHandleUnexpectedExit(
		GuestContext *guest_context) {
		VmmpDumpGuestSelectors();
		const VmExitInformation exit_reason = { static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitReason)) };
		HYPERPLATFORM_LOG_DEBUG("%x", exit_reason.fields.reason);
		HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnexpectedVmExit,
			reinterpret_cast<ULONG_PTR>(guest_context), 0,
			0);
	}

	// MTF VM-exit
	_Use_decl_annotations_ static void VmmpHandleMonitorTrap(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		auto processor_data = guest_context->stack->processor_data;
		ShHandleMonitorTrapFlag(processor_data->sh_data,
			processor_data->shared_data->shared_sh_data,
			processor_data->ept_data);
	}

	//exception handler #PF, #GP, #BP, #TF
	_Use_decl_annotations_ static void VmmpHandleException(GuestContext *guest_context)
	{
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		const VmExitInterruptionInformationField exception = {
			static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitIntrInfo))
		};

		if (static_cast<interruption_type>(exception.fields.interruption_type) == interruption_type::kHardwareException)
		{
			//Hardware exception
			if (static_cast<InterruptionVector>(exception.fields.vector) == InterruptionVector::kPageFaultException)
			{
				// #PF
				const PageFaultErrorCode fault_code = {
					static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitIntrErrorCode))
				};

				//Exit qualification
				const auto fault_address = UtilVmRead(VmcsField::kExitQualification);

				VmEntryInterruptionInformationField inject = {};
				inject.fields.interruption_type = exception.fields.interruption_type;
				inject.fields.vector = exception.fields.vector;
				inject.fields.deliver_error_code = true;
				inject.fields.valid = true;

				AsmWriteCR2(fault_address);
				UtilVmWrite(VmcsField::kVmEntryExceptionErrorCode, fault_code.all);
				UtilVmWrite(VmcsField::kVmEntryIntrInfoField, inject.all); 

			}
			else if (static_cast<InterruptionVector>(exception.fields.vector) == InterruptionVector::kGeneralProtectionException)
			{
				// # GP
				const auto error_code = static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitIntrErrorCode));

				VmEntryInterruptionInformationField inject = {};
				inject.fields.interruption_type = exception.fields.interruption_type;
				inject.fields.vector = exception.fields.vector;
				inject.fields.deliver_error_code = true;
				inject.fields.valid = true;
				UtilVmWrite(VmcsField::kVmEntryExceptionErrorCode, error_code);
				UtilVmWrite(VmcsField::kVmEntryIntrInfoField, inject.all);
				// HYPERPLATFORM_LOG_DEBUG("[#GP]GuestIp= %p, #GP Code= 0x%2x", guest_context->ip, error_code);

			}
			else
			{
				//InterruptionVector EXIT = static_cast<InterruptionVector>(exception.fields.vector); 
				HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0, 0,
				                               0);
			}
		}
		else if (static_cast<interruption_type>(exception.fields.interruption_type) == interruption_type::kSoftwareException)
		{
			if (static_cast<InterruptionVector>(exception.fields.vector) == InterruptionVector::kBreakpointException) {
				// #BP

				if (ShHandleBreakpoint(
					guest_context->stack->processor_data->sh_data,
					guest_context->stack->processor_data->shared_data->shared_sh_data,
					reinterpret_cast<void *>(guest_context->ip))) {
					return;
				}

				VmEntryInterruptionInformationField inject = {};
				inject.fields.interruption_type = exception.fields.interruption_type;
				inject.fields.vector = exception.fields.vector;
				inject.fields.deliver_error_code = false;
				inject.fields.valid = true;
				UtilVmWrite(VmcsField::kVmEntryIntrInfoField, inject.all);
				UtilVmWrite(VmcsField::kVmEntryInstructionLen, 1);

				HYPERPLATFORM_LOG_DEBUG("L0 GuestIp= %p, #BP vmcs no.: %d ", guest_context->ip, UtilVmRead(VmcsField::kVirtualProcessorId));

			}
			else if (static_cast<InterruptionVector>(exception.fields.vector) == InterruptionVector::kTrapFlags)
			{  
				HYPERPLATFORM_LOG_DEBUG("L0 GuestIp= %p, #DB vmcs no.: %d ", guest_context->ip, UtilVmRead(VmcsField::kVirtualProcessorId));

			}
			else {
				HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0, 0,
					0);
			}
		}
		else {
			HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0, 0,
				0);
		}
	} 
	 // CPUID
	_Use_decl_annotations_ static void VmmpHandleCpuid(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		unsigned int cpu_info[4] = {};
		const auto function_id = static_cast<int>(guest_context->gp_regs->ax);
		const auto sub_function_id = static_cast<int>(guest_context->gp_regs->cx);

		if (function_id == 0 && sub_function_id == kHyperPlatformVmmBackdoorCode) {
			// Say "Pong by VMM!" when the back-door code was given
			guest_context->gp_regs->bx = 'gnoP';
			guest_context->gp_regs->dx = ' yb ';
			guest_context->gp_regs->cx = '!MMV';
		}
		else {
			__cpuidex(reinterpret_cast<int *>(cpu_info), function_id, sub_function_id);
			guest_context->gp_regs->ax = cpu_info[0];
			guest_context->gp_regs->bx = cpu_info[1];
			guest_context->gp_regs->cx = cpu_info[2];
			guest_context->gp_regs->dx = cpu_info[3];
		}

		VmmpAdjustGuestInstructionPointer(guest_context->ip);
	}

	// RDTSC
	_Use_decl_annotations_ static void VmmpHandleRdtsc(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		ULARGE_INTEGER tsc = {};
		tsc.QuadPart = __rdtsc();
		guest_context->gp_regs->dx = tsc.HighPart;
		guest_context->gp_regs->ax = tsc.LowPart;

		VmmpAdjustGuestInstructionPointer(guest_context->ip);
	}

	// RDTSCP
	_Use_decl_annotations_ static void VmmpHandleRdtscp(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		unsigned int tsc_aux = 0;
		ULARGE_INTEGER tsc = {};
		tsc.QuadPart = __rdtscp(&tsc_aux);
		guest_context->gp_regs->dx = tsc.HighPart;
		guest_context->gp_regs->ax = tsc.LowPart;
		guest_context->gp_regs->cx = tsc_aux;

		VmmpAdjustGuestInstructionPointer(guest_context->ip);
	}

	// XSETBV. It is executed at the time of system resuming
	_Use_decl_annotations_ static void VmmpHandleXsetbv(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		ULARGE_INTEGER value = {};
		value.LowPart = static_cast<ULONG>(guest_context->gp_regs->ax);
		value.HighPart = static_cast<ULONG>(guest_context->gp_regs->dx);
		_xsetbv(static_cast<ULONG>(guest_context->gp_regs->cx), value.QuadPart);

		VmmpAdjustGuestInstructionPointer(guest_context->ip);
	}

	// RDMSR
	_Use_decl_annotations_ static void VmmpHandleMsrReadAccess(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		VmmpHandleMsrAccess(guest_context, true);
	}

	// WRMSR
	_Use_decl_annotations_ static void VmmpHandleMsrWriteAccess(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		VmmpHandleMsrAccess(guest_context, false);
	}

	// RDMSR and WRMSR
	_Use_decl_annotations_ static void VmmpHandleMsrAccess(
		GuestContext *guest_context, bool read_access) {
		// Apply it for VMCS instead of a real MSR if a speficied MSR is either of
		// them.
		const auto msr = static_cast<Msr>(guest_context->gp_regs->cx);

		bool transfer_to_vmcs = false;
		VmcsField vmcs_field = {};
		switch (msr) {
		case Msr::kIa32SysenterCs:
			vmcs_field = VmcsField::kGuestSysenterCs;
			transfer_to_vmcs = true;
			break;
		case Msr::kIa32SysenterEsp:
			vmcs_field = VmcsField::kGuestSysenterEsp;
			transfer_to_vmcs = true;
			break;
		case Msr::kIa32SysenterEip:
			vmcs_field = VmcsField::kGuestSysenterEip;
			transfer_to_vmcs = true;
			break;
		case Msr::kIa32GsBase:
			vmcs_field = VmcsField::kGuestGsBase;
			transfer_to_vmcs = true;
			break;
		case Msr::kIa32FsBase:
			vmcs_field = VmcsField::kGuestFsBase;
			break;
		default:
			break;
		}
		// Do not shadow 64bit fields because the current implmentation for x86 is not
		// able to handle it due to a simple use of UtilVmWrite() below.
		NT_ASSERT(UtilIsInBounds(vmcs_field, VmcsField::kIoBitmapA,
			VmcsField::kHostIa32PerfGlobalCtrlHigh) == false);

		LARGE_INTEGER msr_value = {};
		if (read_access) {
			if (transfer_to_vmcs)
			{
				msr_value.QuadPart = UtilVmRead(vmcs_field);
			}
			else
			{
				msr_value.QuadPart = UtilReadMsr64(msr);
			}
			guest_context->gp_regs->ax = msr_value.LowPart;
			guest_context->gp_regs->dx = msr_value.HighPart;
		}
		else
		{
			msr_value.LowPart = static_cast<ULONG>(guest_context->gp_regs->ax);
			msr_value.HighPart = static_cast<ULONG>(guest_context->gp_regs->dx);
			if (transfer_to_vmcs)
			{
				UtilVmWrite(vmcs_field, static_cast<ULONG_PTR>(msr_value.QuadPart));
			}
			else
			{
				UtilWriteMsr64(msr, msr_value.QuadPart);
			}
		}

		VmmpAdjustGuestInstructionPointer(guest_context->ip);
	}

	// LIDT, SIDT, LGDT and SGDT
	_Use_decl_annotations_ static void VmmpHandleGdtrOrIdtrAccess(GuestContext *guest_context)
	{
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		const GdtrOrIdtrAccessQualification exit_qualification = {
			static_cast<ULONG32>(UtilVmRead(VmcsField::kVmxInstructionInfo)) };

		// Calculate an address to be used for the instruction
		const auto displacement = UtilVmRead(VmcsField::kExitQualification);

		// Base
		ULONG_PTR base_value = 0;
		if (!exit_qualification.fields.base_register_invalid) {
			const auto register_used = VmmpSelectRegister(
				exit_qualification.fields.base_register, guest_context);
			base_value = *register_used;
		}

		// Index
		ULONG_PTR index_value = 0;
		if (!exit_qualification.fields.index_register_invalid) {
			const auto register_used = VmmpSelectRegister(
				exit_qualification.fields.index_register, guest_context);
			index_value = *register_used;
			switch (
				static_cast<GdtrOrIdtrScaling>(exit_qualification.fields.scalling)) {
			case GdtrOrIdtrScaling::kNoScaling:
				index_value = index_value;
				break;
			case GdtrOrIdtrScaling::kScaleBy2:
				index_value = index_value * 2;
				break;
			case GdtrOrIdtrScaling::kScaleBy4:
				index_value = index_value * 4;
				break;
			case GdtrOrIdtrScaling::kScaleBy8:
				index_value = index_value * 8;
				break;
			default:
				break;
			}
		}

		auto operation_address = base_value + index_value + displacement;
		if (static_cast<GdtrOrIdtrAaddressSize>(
			exit_qualification.fields.address_size) ==
			GdtrOrIdtrAaddressSize::k32bit) {
			operation_address &= MAXULONG;
		}

		// Update CR3 with that of the guest since below code is going to access
		// memory.
		const auto guest_cr3 = UtilVmRead(VmcsField::kGuestCr3);
		const auto vmm_cr3 = __readcr3();
		__writecr3(guest_cr3);

		// Emulate the instruction
		auto descriptor_table_reg = reinterpret_cast<Idtr *>(operation_address);
		switch (static_cast<GdtrOrIdtrInstructionIdentity>(
			exit_qualification.fields.instruction_identity)) {
		case GdtrOrIdtrInstructionIdentity::kSgdt:
			descriptor_table_reg->base = UtilVmRead(VmcsField::kGuestGdtrBase);
			descriptor_table_reg->limit =
				static_cast<unsigned short>(UtilVmRead(VmcsField::kGuestGdtrLimit));
			break;
		case GdtrOrIdtrInstructionIdentity::kSidt:
			descriptor_table_reg->base = UtilVmRead(VmcsField::kGuestIdtrBase);
			descriptor_table_reg->limit =
				static_cast<unsigned short>(UtilVmRead(VmcsField::kGuestIdtrLimit));
			break;
		case GdtrOrIdtrInstructionIdentity::kLgdt:
			UtilVmWrite(VmcsField::kGuestGdtrBase, descriptor_table_reg->base);
			UtilVmWrite(VmcsField::kGuestGdtrLimit, descriptor_table_reg->limit);
			break;
		case GdtrOrIdtrInstructionIdentity::kLidt:
			UtilVmWrite(VmcsField::kGuestIdtrBase, descriptor_table_reg->base);
			UtilVmWrite(VmcsField::kGuestIdtrLimit, descriptor_table_reg->limit);
			break;
		}

		__writecr3(vmm_cr3);
		VmmpAdjustGuestInstructionPointer(guest_context->ip);
	}

	// LLDT, LTR, SLDT, and STR
	_Use_decl_annotations_ static void VmmpHandleLdtrOrTrAccess(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		const LdtrOrTrAccessQualification exit_qualification = {
			static_cast<ULONG32>(UtilVmRead(VmcsField::kVmxInstructionInfo)) };

		// Calculate an address or a register to be used for the instruction
		const auto displacement = UtilVmRead(VmcsField::kExitQualification);

		ULONG_PTR operation_address = 0;
		if (exit_qualification.fields.register_access) {
			// Register
			const auto register_used =
				VmmpSelectRegister(exit_qualification.fields.register1, guest_context);
			operation_address = reinterpret_cast<ULONG_PTR>(register_used);
		}
		else {
			// Base
			ULONG_PTR base_value = 0;
			if (!exit_qualification.fields.base_register_invalid) {
				const auto register_used = VmmpSelectRegister(
					exit_qualification.fields.base_register, guest_context);
				base_value = *register_used;
			}

			// Index
			ULONG_PTR index_value = 0;
			if (!exit_qualification.fields.index_register_invalid) {
				const auto register_used = VmmpSelectRegister(
					exit_qualification.fields.index_register, guest_context);
				index_value = *register_used;
				switch (
					static_cast<GdtrOrIdtrScaling>(exit_qualification.fields.scalling)) {
				case GdtrOrIdtrScaling::kNoScaling:
					index_value = index_value;
					break;
				case GdtrOrIdtrScaling::kScaleBy2:
					index_value = index_value * 2;
					break;
				case GdtrOrIdtrScaling::kScaleBy4:
					index_value = index_value * 4;
					break;
				case GdtrOrIdtrScaling::kScaleBy8:
					index_value = index_value * 8;
					break;
				default:
					break;
				}
			}

			operation_address = base_value + index_value + displacement;
			if (static_cast<GdtrOrIdtrAaddressSize>(
				exit_qualification.fields.address_size) ==
				GdtrOrIdtrAaddressSize::k32bit) {
				operation_address &= MAXULONG;
			}
		}

		// Update CR3 with that of the guest since below code is going to access
		// memory.
		const auto guest_cr3 = UtilVmRead(VmcsField::kGuestCr3);
		const auto vmm_cr3 = __readcr3();
		__writecr3(guest_cr3);

		// Emulate the instruction
		auto selector = reinterpret_cast<USHORT *>(operation_address);
		switch (static_cast<LdtrOrTrInstructionIdentity>(
			exit_qualification.fields.instruction_identity)) {
		case LdtrOrTrInstructionIdentity::kSldt:
			*selector =
				static_cast<USHORT>(UtilVmRead(VmcsField::kGuestLdtrSelector));
			break;
		case LdtrOrTrInstructionIdentity::kStr:
			*selector = static_cast<USHORT>(UtilVmRead(VmcsField::kGuestTrSelector));
			break;
		case LdtrOrTrInstructionIdentity::kLldt:
			UtilVmWrite(VmcsField::kGuestLdtrSelector, *selector);
			break;
		case LdtrOrTrInstructionIdentity::kLtr:
			UtilVmWrite(VmcsField::kGuestTrSelector, *selector);
			break;
		}

		__writecr3(vmm_cr3);
		VmmpAdjustGuestInstructionPointer(guest_context->ip);
	}

	// MOV to / from DRx
	_Use_decl_annotations_ static void VmmpHandleDrAccess(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		const MovDrQualification exit_qualification = {
			UtilVmRead(VmcsField::kExitQualification) };
		const auto register_used =
			VmmpSelectRegister(exit_qualification.fields.gp_register, guest_context);

		// Emulate the instruction
		switch (static_cast<MovDrDirection>(exit_qualification.fields.direction)) {
		case MovDrDirection::kMoveToDr:
			// clang-format off
			switch (exit_qualification.fields.debugl_register)
			{
			case 0: __writedr(0, *register_used); break;
			case 1: __writedr(1, *register_used); break;
			case 2: __writedr(2, *register_used); break;
			case 3: __writedr(3, *register_used); break;
			case 4: __writedr(4, *register_used); break;
			case 5: __writedr(5, *register_used); break;
			case 6: __writedr(6, *register_used); break;
			case 7: __writedr(7, *register_used); break;
			default: break;
			}
			// clang-format on
			break;
		case MovDrDirection::kMoveFromDr:
			// clang-format off
			switch (exit_qualification.fields.debugl_register)
			{
			case 0: *register_used = __readdr(0); break;
			case 1: *register_used = __readdr(1); break;
			case 2: *register_used = __readdr(2); break;
			case 3: *register_used = __readdr(3); break;
			case 4: *register_used = __readdr(4); break;
			case 5: *register_used = __readdr(5); break;
			case 6: *register_used = __readdr(6); break;
			case 7: *register_used = __readdr(7); break;
			default: break;
			}
			// clang-format on
			break;
		default:
			HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0, 0,
				0);
			break;
		}

		VmmpAdjustGuestInstructionPointer(guest_context->ip);
	}

	// MOV to / from CRx
	_Use_decl_annotations_ static void VmmpHandleCrAccess(GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		const MovCrQualification exit_qualification = {
			UtilVmRead(VmcsField::kExitQualification)
		};

		const auto register_used =
			VmmpSelectRegister(exit_qualification.fields.gp_register, guest_context);

		switch (static_cast<MovCrAccessType>(exit_qualification.fields.access_type)) {
		case MovCrAccessType::kMoveToCr:
			switch (exit_qualification.fields.control_register) {
				// CR0 <- Reg
			case 0: {
				HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
				if (UtilIsX86Pae()) {
					UtilLoadPdptes(UtilVmRead(VmcsField::kGuestCr3));
				}
				UtilVmWrite(VmcsField::kGuestCr0, *register_used);
				UtilVmWrite(VmcsField::kCr0ReadShadow, *register_used);
				break;
			}

					// CR3 <- Reg
			case 3: {
				HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
				if (UtilIsX86Pae()) {
					UtilLoadPdptes(*register_used);
				}
				UtilVmWrite(VmcsField::kGuestCr3, *register_used);

				break;
			}

					// CR4 <- Reg
			case 4: {
				HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
				if (UtilIsX86Pae()) {
					UtilLoadPdptes(UtilVmRead(VmcsField::kGuestCr3));
				}
				UtilVmWrite(VmcsField::kGuestCr4, *register_used);
				UtilVmWrite(VmcsField::kCr4ReadShadow, *register_used);
				break;
			}

					// CR8 <- Reg
			case 8: {
				HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
				guest_context->cr8 = *register_used;
				break;
			}

			default:
				HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0,
					0, 0);
				break;
			}
			break;

		case MovCrAccessType::kMoveFromCr:
			switch (exit_qualification.fields.control_register) {
				// Reg <- CR3
			case 3: {
				HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
				*register_used = UtilVmRead(VmcsField::kGuestCr3);

				break;
			}

					// Reg <- CR8
			case 8: {
				HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
				*register_used = guest_context->cr8;
				break;
			}

			default:
				HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0,
					0, 0);
				break;
			}
			break;

			// Unimplemented
		case MovCrAccessType::kClts:
		case MovCrAccessType::kLmsw:
		default:
			HYPERPLATFORM_COMMON_DBG_BREAK();
			break;
		}

		VmmpAdjustGuestInstructionPointer(guest_context->ip);
	}

	//-----------------------------------------------------------------------------------------------------------------//
	/*
	VMsucceed:
	CF ← 0;
	PF ← 0;
	AF ← 0;
	ZF ← 0;
	SF ← 0;
	OF ← 0;
	*/
	VOID VMSucceed(FlagRegister *reg)
	{
		reg->fields.cf = false;  // Error without status
		reg->fields.pf = false;
		reg->fields.af = false;
		reg->fields.zf = false;  // Error without status
		reg->fields.sf = false;
		reg->fields.of = false;
	}
	//-----------------------------------------------------------------------------------------------------------------------------------------------------//
	/*
	VMfailInvalid:
	CF ← 1;
	PF ← 0;
	AF ← 0;
	ZF ← 0;
	SF ← 0;
	OF ← 0;
	*/
	VOID VMfailInvalid(FlagRegister *reg)
	{
		reg->fields.cf = true;  // Error without status
		reg->fields.pf = false;
		reg->fields.af = false;
		reg->fields.zf = false;  // Error without status
		reg->fields.sf = false;
		reg->fields.of = false;
	}

	//-----------------------------------------------------------------------------------------------------------------------------------------------------//
	/*
	VMfailValid(ErrorNumber):// executed only if there is a current VMCS
	CF ← 0;
	PF ← 0;
	AF ← 0;
	ZF ← 1;
	SF ← 0;
	OF ← 0;
	Set the VM-instruction error field to ErrorNumber;
	*/
	VOID VMfailValid(FlagRegister *reg, VmxInstructionError err)
	{
		UNREFERENCED_PARAMETER(err);
		reg->fields.cf = false;  // Error without status
		reg->fields.pf = false;
		reg->fields.af = false;
		reg->fields.zf = true;  // Error without status
		reg->fields.sf = false;
		reg->fields.of = false;
		//	Set the VM-instruction error field to ErrorNumber;
	}

	//-------------------------------------------------------------------------------------------------------------------------------------------------------//

	VOID VMfail(FlagRegister *reg, VmxInstructionError err)
	{
		UNREFERENCED_PARAMETER(err);
		//if and only if VMCS is current
		//VMfailValid(reg, err);
		//else
		VMfailInvalid(reg);
	}

	//----------------------------------------------------------------------------------------------------------------//
	BOOLEAN IsGuestinPagingMode()
	{
		Cr0 cr0 = { UtilVmRead64(VmcsField::kGuestCr0) };
		return (cr0.fields.pg) ? TRUE : FALSE;
	}

	//----------------------------------------------------------------------------------------------------------------//
	BOOLEAN IsGuestSetNumericErrorBit()
	{
		Cr0 cr0 = { UtilVmRead64(VmcsField::kGuestCr0) };
		return (cr0.fields.ne) ? TRUE : FALSE;
	}

	//----------------------------------------------------------------------------------------------------------------//
	BOOLEAN IsGuestInProtectedMode()
	{
		Cr0 cr0 = { UtilVmRead64(VmcsField::kGuestCr0) };
		return (cr0.fields.pe) ? TRUE : FALSE;
	}

	//----------------------------------------------------------------------------------------------------------------//
	BOOLEAN IsGuestSupportVMX()
	{
		Cr4 cr4 = { UtilVmRead64(VmcsField::kGuestCr4) };
		return (cr4.fields.vmxe) ? TRUE : FALSE;
	}

	//----------------------------------------------------------------------------------------------------------------//
	BOOLEAN IsGuestInVirtual8086()
	{
		FlagRegister flags = { UtilVmRead64(VmcsField::kGuestRflags) };
		return (flags.fields.vm) ? TRUE : FALSE;
	}

	//----------------------------------------------------------------------------------------------------------------//
	SegmentDesctiptor* GetSegmentDesctiptor(SegmentSelector ss, ULONG64 gdtBase)
	{
		return	reinterpret_cast<SegmentDesctiptor *>(gdtBase + ss.fields.index * sizeof(SegmentDesctiptor));
	}


	//----------------------------------------------------------------------------------------------------------------//
	BOOLEAN IsGuestinCompatibliltyMode()
	{
		SegmentSelector ss = { UtilVmRead64(VmcsField::kGuestCsSelector) };
		ULONG64 gdtBase = UtilVmRead64(VmcsField::kGuestGdtrBase);
		SegmentDesctiptor* ds = GetSegmentDesctiptor(ss, gdtBase);
		return (ds->fields.l) ? TRUE : FALSE;
	}


	//----------------------------------------------------------------------------------------------------------------//
	USHORT GetGuestCPL()
	{
		const SegmentSelector ss = { UtilVmRead64(VmcsField::kGuestCsSelector) };
		USHORT ret = ss.fields.rpl;
		return ret;
	}

	//----------------------------------------------------------------------------------------------------------------//
	BOOLEAN IsGuestInIA32eMode()
	{
		MSR_EFER efer = { UtilVmRead64(VmcsField::kGuestIa32Efer) };
		return (efer.fields.LMA) ? TRUE : FALSE;
	}

	//----------------------------------------------------------------------------------------------------------------//
	///See: Layout of IA32_FEATURE_CONTROL
	BOOLEAN IsLockbitClear()
	{
		Ia32FeatureControlMsr vmx_feature_control = { UtilReadMsr64(Msr::kIa32FeatureControl) };
		if (vmx_feature_control.fields.lock) {
			return TRUE;
		}
		return FALSE;
	}
	//----------------------------------------------------------------------------------------------------------------//
	///See:  Layout of IA32_FEATURE_CONTROL
	BOOLEAN IsGuestEnableVMXOnInstruction()
	{
		Ia32FeatureControlMsr vmx_feature_control = { UtilReadMsr64(Msr::kIa32FeatureControl) };
		if (vmx_feature_control.fields.enable_vmxon) {
			return TRUE;
		}
		return FALSE;
	}
	//----------------------------------------------------------------------------------------------------------------//
	BOOLEAN CheckPhysicalAddress(ULONG64 vmxon_region_pa)
	{
		Ia32VmxBasicMsr vmx_basic = { UtilReadMsr64(Msr::kIa32VmxBasic) };
		if (vmx_basic.fields.supported_ia64)
		{
			//0xFFFFFFFF00001234 & 0xFFFFFFFF00000000 != 0
			if ((CHECK_BOUNDARY_FOR_IA32 & vmxon_region_pa) != 0)
			{
				return FALSE;
			}
		}
		return TRUE;
	}

	//----------------------------------------------------------------------------------------------------------------//
	ULONG GetVMCSRevisionIdentifier()
	{
		Ia32VmxBasicMsr vmx_basic = { UtilReadMsr64(Msr::kIa32VmxBasic) };
		return vmx_basic.fields.revision_identifier;
	}




	//----------------------------------------------------------------------------------------------------------------//
	BOOLEAN CheckPageAlgined(ULONG64 address)
	{
		//i.e. 0x22341000 & 0xFFF = 0 , it is page aligned ,
		//	   0x22342355 &	0xFFF = 0x355 , it is not aglined
		return ((address & CHECK_PAGE_ALGINMENT) == 0);
	}


	//----------------------------------------------------------------------------------------------------------------//
	VOID FillEventInjection(ULONG32 interruption_type, ULONG32 exception_vector, BOOLEAN isDeliver_error_code, BOOLEAN isValid)
	{
		VmEntryInterruptionInformationField inject = {};
		inject.fields.interruption_type = interruption_type;
		inject.fields.vector = exception_vector;
		inject.fields.deliver_error_code = isDeliver_error_code;
		inject.fields.valid = isValid;
		UtilVmWrite(VmcsField::kVmEntryIntrInfoField, inject.all);
	}

	//----------------------------------------------------------------------------------------------------------------//
	VOID ThrowInvalidCodeException()
	{
		FillEventInjection((ULONG32)interruption_type::kHardwareException, (ULONG32)InterruptionVector::kInvalidOpcode, FALSE, TRUE);
	}


	//----------------------------------------------------------------------------------------------------------------//
	VOID ThrowGerneralFaultInterrupt()
	{
		FillEventInjection((ULONG32)interruption_type::kHardwareException, (ULONG32)InterruptionVector::kGeneralProtectionException, FALSE, TRUE);
	}


	ULONG64 DecodeOrVmclearOrVmptrldOrVmptrstOrVmxon(GuestContext* guest_context)
	{
		const VMInstructionQualificationForClearOrPtrldOrPtrstOrVmxon exit_qualification =
		{
			static_cast<ULONG32>(UtilVmRead(VmcsField::kVmxInstructionInfo))
		};

		// Calculate an address to be used for the instruction
		const auto displacement = UtilVmRead(VmcsField::kExitQualification);
		// Base
		ULONG_PTR base_value = 0;
		if (!exit_qualification.fields.BaseRegInvalid)
		{
			const auto register_used = VmmpSelectRegister(exit_qualification.fields.BaseReg, guest_context);
			base_value = *register_used;
		}

		// Index
		ULONG_PTR index_value = 0;

		if (!exit_qualification.fields.IndexRegInvalid)
		{
			const auto register_used = VmmpSelectRegister(exit_qualification.fields.IndxeReg, guest_context);

			index_value = *register_used;
			switch (static_cast<VMXScaling>(exit_qualification.fields.scalling))
			{
			case VMXScaling::kNoScaling:
				index_value = index_value;
				break;
			case VMXScaling::kScaleBy2:
				index_value = index_value * 2;
				break;
			case VMXScaling::kScaleBy4:
				index_value = index_value * 4;
				break;
			case VMXScaling::kScaleBy8:
				index_value = index_value * 8;
				break;
			default:
				break;
			}
		}

		auto operation_address = base_value + index_value + displacement;

		if (static_cast<VMXAaddressSize>(exit_qualification.fields.address_size) == VMXAaddressSize::k32bit)
		{
			operation_address &= MAXULONG;
		}
		const auto guest_cr3 = UtilVmRead(VmcsField::kGuestCr3);
		const auto vmm_cr3 = __readcr3();
		HYPERPLATFORM_LOG_DEBUG("operation_address= %I64x + %I64x + %I64x = %I64x \r\n", base_value, index_value, displacement, operation_address);
		HYPERPLATFORM_COMMON_DBG_BREAK();
		return operation_address;

	}


	//----------------------------------------------------------------------------------------------------------------//




	//What functions we support for nested 
	void init_vmx_extensions_bitmask(void)
	{
		g_vmx_extensions_bitmask |=
			MY_VMX_VIRTUAL_NMI |
			MY_VMX_TPR_SHADOW |
			MY_VMX_APIC_VIRTUALIZATION |
			MY_VMX_WBINVD_VMEXIT |
			MY_VMX_PREEMPTION_TIMER |
			MY_VMX_PAT |
			MY_VMX_EFER |
			MY_VMX_EPT |
			MY_VMX_VPID |
			MY_VMX_UNRESTRICTED_GUEST |
			MY_VMX_DESCRIPTOR_TABLE_EXIT |
			MY_VMX_X2APIC_VIRTUALIZATION |
			MY_VMX_PAUSE_LOOP_EXITING |
			MY_VMX_EPT_ACCESS_DIRTY |
			MY_VMX_VINTR_DELIVERY |
			MY_VMX_VMCS_SHADOWING |
			MY_VMX_EPTP_SWITCHING |
			MY_VMX_EPT_EXCEPTION |
			MY_VMX_SAVE_DEBUGCTL_DISABLE |
			MY_VMX_PERF_GLOBAL_CTRL;

	}


	//Is ept-pointer validate
	BOOLEAN is_eptptr_valid(ULONG64 eptptr)
	{
		// [2:0] EPT paging-structure memory type
		//       0 = Uncacheable (UC)
		//       6 = Write-back (WB)
		ULONG32 memtype = eptptr & 7;
		if (memtype != (ULONG32)memory_type::kUncacheable && memtype != (ULONG32)memory_type::kWriteBack)
			return FALSE;

		// [5:3] This value is 1 less than the EPT page-walk length
		ULONG32 walk_length = (eptptr >> 3) & 7;
		if (walk_length != 3)
			return FALSE;

		// [6]   EPT A/D Enable
		if (!(g_vmx_extensions_bitmask & MY_VMX_EPT_ACCESS_DIRTY))
		{
			if (eptptr & 0x40)
			{
				HYPERPLATFORM_LOG_DEBUG(("is_eptptr_valid: EPTPTR A/D enabled when not supported by CPU"));
				return FALSE;
			}
		}

#define BX_EPTPTR_RESERVED_BITS 0xf80 /* bits 11:7 are reserved */
		if (eptptr & BX_EPTPTR_RESERVED_BITS) {
			HYPERPLATFORM_LOG_DEBUG(("is_eptptr_valid: EPTPTR reserved bits set"));
			return FALSE;
		}

		if (!CheckPhysicalAddress(eptptr))
			return FALSE;
		return TRUE;
	}

	//---------------------------------------------------------------------------------------------------------------------//

	VOID VmxonEmulate(GuestContext* guest_context)
	{
		do
		{
			ULONG64				InstructionPointer = { UtilVmRead64(VmcsField::kGuestRip) };
			ULONG64				StackPointer = { UtilVmRead64(VmcsField::kGuestRsp) };
			ULONG64				vmxon_region_pa = *(PULONG64)DecodeOrVmclearOrVmptrldOrVmptrstOrVmxon(guest_context);
			ULONG64				debug_vmxon_region_pa = DecodeOrVmclearOrVmptrldOrVmptrstOrVmxon(guest_context);
			VmControlStructure* vmxon_region_struct = (VmControlStructure*)UtilVaFromPa(vmxon_region_pa);
			PROCESSOR_NUMBER    number;
			HYPERPLATFORM_LOG_DEBUG("UtilVmRead: %I64X", &UtilVmRead);
			HYPERPLATFORM_LOG_DEBUG("UtilVmRead64: %I64X", &UtilVmRead64);
			HYPERPLATFORM_LOG_DEBUG("UtilVmWrite: %I64X", &UtilVmWrite);
			HYPERPLATFORM_LOG_DEBUG("UtilVmWrite64: %I64X", &UtilVmWrite64);
			HYPERPLATFORM_LOG_DEBUG("VmRead: %I64X", &VmRead16);
			HYPERPLATFORM_LOG_DEBUG("VmRead32: %I64X", &VmRead32);
			HYPERPLATFORM_LOG_DEBUG("VmRead64: %I64X", &VmRead64);
			HYPERPLATFORM_LOG_DEBUG("VmWrite: %I64X", &VmWrite16);
			HYPERPLATFORM_LOG_DEBUG("VmWrite32: %I64X", &VmWrite32);
			HYPERPLATFORM_LOG_DEBUG("VmWrite64: %I64X", &VmWrite64);
			// VMXON_REGION IS NULL
			if (!vmxon_region_pa)
			{
				HYPERPLATFORM_LOG_DEBUG(("VMXON: Parameter is NULL !"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}

			// If already vCPU run in VMX operation
			if (g_vcpus[KeGetCurrentProcessorNumberEx(&number)])
			{
				///TODO: 
				///if( it is non root ) 
				///	VM Exit 
				HYPERPLATFORM_LOG_DEBUG("VMX: Cpu is already in VMXON Mode, should be VM Exit here \r\n");
				break;
			}

			//CR0.PE = 0;
			if (!IsGuestInProtectedMode())
			{
				HYPERPLATFORM_LOG_DEBUG(("VMXON: Please running in Protected Mode !"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}
			//If guest is not support VMX 
			//CR4.VMXE = 0;
			if (!IsGuestSupportVMX())
			{
				HYPERPLATFORM_LOG_DEBUG(("VMXON: Guest is not supported VMX !"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}
			//If guest run in virtual-8086 mode
			//RFLAGS.VM = 1
			if (IsGuestInVirtual8086())
			{
				HYPERPLATFORM_LOG_DEBUG(("VMXON: Guest is running in virtual-8086 mode !"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}
			//If guest run in IA32-e 
			//kGuestIa32Efer.LMA = 1
			if (IsGuestInIA32eMode())
			{
				//If CS.L == 0 , means compability mode (32bit addressing), CS.L == 1 is 64bit mode , default operand is 32bit
				if (!IsGuestinCompatibliltyMode())
				{
					HYPERPLATFORM_LOG_DEBUG(("VMXON: Guest is IA-32e mode but not in 64bit mode !"));
					//#UD
					ThrowInvalidCodeException();
					break;
				}
			}
			//Get Guest CPL
			if (GetGuestCPL() > 0)
			{
				HYPERPLATFORM_LOG_DEBUG(("VMXON: Need run in Ring-0 !"));
				//#gp
				ThrowGerneralFaultInterrupt();
				break;
			}
			//If MSR Lockbit is not set
			//Ia32_Feature_Control.lock = 0
			if (!IsLockbitClear())
			{
				HYPERPLATFORM_LOG_DEBUG(("VMXON: IsLockbitClear !"));
				//#gp
				ThrowGerneralFaultInterrupt();
				break;
			}
			//If guest is not enable VMXON instruction
			//Run outside of SMX mode, and Ia32_Feature_Control.enable_vmxon = 1
			if (!IsGuestEnableVMXOnInstruction())
			{
				HYPERPLATFORM_LOG_DEBUG(("VMXON: Guest is not enable VMXON instruction !"));
				//#gp
				ThrowGerneralFaultInterrupt();
				break;
			}
			//If guest is not set Numberic Error Bit in CR0
			//CR0.NE = 0
			if (!IsGuestSetNumericErrorBit())
			{
				HYPERPLATFORM_LOG_DEBUG(("VMXON: has not set numberic error bit of CR0 register !"));
				//#gp
				ThrowGerneralFaultInterrupt();
				break;
			}
			//if is it not page aglined
			if (!CheckPageAlgined(vmxon_region_pa))
			{
				HYPERPLATFORM_LOG_DEBUG(("VMXON: not page aligned physical address %I64X !"), vmxon_region_pa);
				VMfailInvalid(&guest_context->flag_reg);
				break;
			}
			//if IA32_VMX_BASIC[48] == 1 it is not support 64bit addressing, so address[32] to address[63] supposed = 0
			if (!CheckPhysicalAddress(vmxon_region_pa))
			{
				HYPERPLATFORM_LOG_DEBUG(("VMXON: invalid physical address %I64X !"), vmxon_region_pa);
				VMfailInvalid(&guest_context->flag_reg);
				break;
			}

			//VMCS id is not supported
			if (vmxon_region_struct->revision_identifier != GetVMCSRevisionIdentifier())
			{
				HYPERPLATFORM_LOG_DEBUG(("VMXON: VMCS revision identifier is not supported,  CPU supports identifier is : %x !"), GetVMCSRevisionIdentifier());
				VMfailInvalid(&guest_context->flag_reg);
				break;
			}

			///TODO: a20m and in SMX operation3 and bit 1 of IA32_FEATURE_CONTROL MSR is clear

			NestedVmm* vm = (NestedVmm*)ExAllocatePool(NonPagedPoolNx, sizeof(NestedVmm));
			vm->inVMX = TRUE;
			vm->inRoot = TRUE;
			vm->blockINITsignal = TRUE;
			vm->blockAndDisableA20M = TRUE;
			vm->current_vmcs = 0xFFFFFFFFFFFFFFFF;
			vm->guest_vmcs = 0xFFFFFFFFFFFFFFFF;
			vm->vmxon_region = vmxon_region_pa;
			vm->CpuNumber = KeGetCurrentProcessorNumberEx(&number);
			vm->inRoot = TRUE;
			g_vcpus[vm->CpuNumber] = vm;
			HYPERPLATFORM_LOG_DEBUG("VMXON: Guest Instruction Pointer %I64X Guest Stack Pointer: %I64X  Guest VMXON_Region: %I64X stored at %I64x physical address\r\n",
				InstructionPointer, StackPointer, vmxon_region_pa, debug_vmxon_region_pa);

			HYPERPLATFORM_LOG_DEBUG("VMXON: Run Successfully with VMXON_Region:  %I64X Total Vitrualized Core: %x  Current Cpu: %x in Cpu Group : %x  Number: %x \r\n",
				vmxon_region_pa, g_VM_Core_Count, vm->CpuNumber, number.Group, number.Number);

			HYPERPLATFORM_LOG_DEBUG("VMXON: VCPU No.: %i Mode: %s Current VMCS : %I64X VMXON Region : %I64X  ",
				g_vcpus[vm->CpuNumber]->CpuNumber, (g_vcpus[vm->CpuNumber]->inVMX) ? "VMX" : "No VMX", g_vcpus[vm->CpuNumber]->current_vmcs, g_vcpus[vm->CpuNumber]->vmxon_region);

			//a group of CPU maximum is 64 core
			if (g_VM_Core_Count < 64)
			{
				_InterlockedIncrement(&g_VM_Core_Count);
			}

			BuildGernericVMCSMap();

			VMSucceed(&guest_context->flag_reg);

		} while (FALSE);
	}


	//---------------------------------------------------------------------------------------------------------------------//

	VOID VmclearEmulate(GuestContext* guest_context)
	{
		do
		{
			ULONG64				InstructionPointer = { UtilVmRead64(VmcsField::kGuestRip) };
			ULONG64				StackPointer = { UtilVmRead64(VmcsField::kGuestRsp) };
			ULONG64				vmcs_region_pa = *(PULONG64)DecodeOrVmclearOrVmptrldOrVmptrstOrVmxon(guest_context);//*(PULONG64)(StackPointer + offset);				//May need to be fixed later
			ULONG64				debug_vmcs_region_pa = DecodeOrVmclearOrVmptrldOrVmptrstOrVmxon(guest_context);
			PROCESSOR_NUMBER	procnumber = {};
			VmControlStructure* vmcs_region_va = (VmControlStructure*)UtilVaFromPa(vmcs_region_pa);
			ULONG				vcpu_index = KeGetCurrentProcessorNumberEx(&procnumber);

			//If parameter is NULL
			if (!vmcs_region_pa)
			{
				HYPERPLATFORM_LOG_DEBUG(("VMXCLEAR: Parameter is NULL ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}

			//If vCPU is not run in VMX mode
			if (!g_vcpus[vcpu_index]->inVMX)
			{
				HYPERPLATFORM_LOG_DEBUG(("VMXCLEAR: VMXON is required ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}

			//CR0.PE = 0;
			if (!IsGuestInProtectedMode())
			{
				HYPERPLATFORM_LOG_DEBUG(("VMXCLEAR: Please running in Protected Mode ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}

			//If guest run in virtual-8086 mode
			//RFLAGS.VM = 1
			if (IsGuestInVirtual8086())
			{
				HYPERPLATFORM_LOG_DEBUG(("VMXCLEAR: Guest is running in virtual-8086 mode ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}

			//If guest run in IA32-e 
			//kGuestIa32Efer.LMA = 1
			if (IsGuestInIA32eMode())
			{
				//If CS.L == 0 , means compability mode (32bit addressing), CS.L == 1 is 64bit mode , default operand is 32bit
				if (!IsGuestinCompatibliltyMode())
				{
					HYPERPLATFORM_LOG_DEBUG(("VMXCLEAR: Guest is IA-32e mode but not in 64bit mode ! \r\n"));
					//#UD
					ThrowInvalidCodeException();
					break;
				}
			}

			//Get Guest CPL
			if (GetGuestCPL() > 0)
			{
				HYPERPLATFORM_LOG_DEBUG(("VMXCLEAR: Need running in Ring - 0 ! \r\n")); 	  //#gp
				ThrowGerneralFaultInterrupt();
				break;
			}

			//if is it not page aglined
			if (!CheckPageAlgined(vmcs_region_pa))
			{
				HYPERPLATFORM_LOG_DEBUG(("VMXCLEAR: not page aligned physical address %I64X ! \r\n"),
					vmcs_region_pa);

				VMfailInvalid(&guest_context->flag_reg);
				break;
			}

			//if IA32_VMX_BASIC[48] == 1 it is not support 64bit addressing, so address[32] to address[63] supposed = 0
			if (!CheckPhysicalAddress(vmcs_region_pa))
			{
				HYPERPLATFORM_LOG_DEBUG(("VMXCLEAR: invalid physical address %I64X ! \r\n"),
					vmcs_region_pa);

				VMfailInvalid(&guest_context->flag_reg);
				break;
			}
			//if vmcs != vmregion 
			if (g_vcpus[vcpu_index] && (vmcs_region_pa == g_vcpus[vcpu_index]->vmxon_region))
			{
				HYPERPLATFORM_LOG_DEBUG(("VMXCLEAR: VMCS region %I64X same as VMXON region %I64X ! \r\n"),
					vmcs_region_pa, g_vcpus[vcpu_index]->vmxon_region);

				VMfailInvalid(&guest_context->flag_reg);
				break;
			}

			*(PLONG)(&vmcs_region_va->data) = VMCS_STATE_CLEAR;
			if (vmcs_region_pa == g_vcpus[vcpu_index]->guest_vmcs)
			{
				g_vcpus[vcpu_index]->guest_vmcs = 0xFFFFFFFFFFFFFFFF;
			}
			__vmx_vmclear(&g_vcpus[vcpu_index]->current_vmcs);

			g_vcpus[vcpu_index]->current_vmcs = 0xFFFFFFFFFFFFFFFF;

			HYPERPLATFORM_LOG_DEBUG("VMCLEAR: Guest Instruction Pointer %I64X Guest Stack Pointer: %I64X  Guest vmcs region: %I64X stored at %I64x on stack\r\n",
				InstructionPointer, StackPointer, vmcs_region_pa, debug_vmcs_region_pa);

			HYPERPLATFORM_LOG_DEBUG("VMCLEAR: Run Successfully with VMCS_Region:  %I64X Total Vitrualized Core: %x  Current Cpu: %x in Cpu Group : %x  Number: %x \r\n",
				vmcs_region_pa, g_VM_Core_Count, g_vcpus[vcpu_index]->CpuNumber, procnumber.Group, procnumber.Number);

			HYPERPLATFORM_LOG_DEBUG("VMCLEAR: VCPU No.: %i Mode: %s Current VMCS : %I64X VMXON Region : %I64X  ",
				g_vcpus[vcpu_index]->CpuNumber, (g_vcpus[vcpu_index]->inVMX) ? "VMX" : "No VMX", g_vcpus[vcpu_index]->current_vmcs, g_vcpus[vcpu_index]->vmxon_region);

			VMSucceed(&guest_context->flag_reg);
		} while (FALSE);
	}

	//---------------------------------------------------------------------------------------------------------------------//

	VOID VmptrldEmulate(GuestContext* guest_context)
	{
		do
		{			
			PROCESSOR_NUMBER	procnumber = {};
			ULONG64				InstructionPointer = { UtilVmRead64(VmcsField::kGuestRip) };
			ULONG64				StackPointer = { UtilVmRead64(VmcsField::kGuestRsp) };
			ULONG64				vmcs_region_pa = *(PULONG64)DecodeOrVmclearOrVmptrldOrVmptrstOrVmxon(guest_context);
			VmControlStructure* vmcs_region_va = (VmControlStructure*)UtilVaFromPa(vmcs_region_pa);
			ULONG				vcpu_index = KeGetCurrentProcessorNumberEx(&procnumber);
			// if vmcs region is NULL
			if (!vmcs_region_pa)
			{
				HYPERPLATFORM_LOG_DEBUG(("kVmptrld: Parameter is NULL ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}
			// if vCPU not run in VMX mode 
			if (!g_vcpus[vcpu_index]->inVMX)
			{
				HYPERPLATFORM_LOG_DEBUG(("kVmptrld: VMXON is required ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}
			//CR0.PE = 0;
			if (!IsGuestInProtectedMode())
			{
				HYPERPLATFORM_LOG_DEBUG(("kVmptrld: Please running in Protected Mode ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}

			//If guest run in virtual-8086 mode
			//RFLAGS.VM = 1
			if (IsGuestInVirtual8086())
			{
				HYPERPLATFORM_LOG_DEBUG(("kVmptrld: Guest is running in virtual-8086 mode ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}

			//If guest run in IA32-e 
			//kGuestIa32Efer.LMA = 1
			if (IsGuestInIA32eMode())
			{
				//If CS.L == 0 , means compability mode (32bit addressing), CS.L == 1 is 64bit mode , default operand is 32bit
				if (!IsGuestinCompatibliltyMode())
				{
					HYPERPLATFORM_LOG_DEBUG(("kVmptrld: Guest is IA-32e mode but not in 64bit mode ! \r\n"));
					//#UD
					ThrowInvalidCodeException();
					break;
				}
			}

			///TODO: If in VMX non-root operation, should be VM Exit

			//Get Guest CPL
			if (GetGuestCPL() > 0)
			{
				HYPERPLATFORM_LOG_DEBUG(("kVmptrld: Need running in Ring - 0 ! \r\n")); 	  //#gp
				ThrowGerneralFaultInterrupt();
				break;
			}
			//if is it not page aglined
			if (!CheckPageAlgined(vmcs_region_pa))
			{
				HYPERPLATFORM_LOG_DEBUG(("kVmptrld: not page aligned physical address %I64X ! \r\n"),
					vmcs_region_pa);

				VMfailInvalid(&guest_context->flag_reg);
				break;
			}

			//if IA32_VMX_BASIC[48] == 1 it is not support 64bit addressing, so address[32] to address[63] supposed = 0
			if (!CheckPhysicalAddress(vmcs_region_pa))
			{
				HYPERPLATFORM_LOG_DEBUG(("kVmptrld: invalid physical address %I64X ! \r\n"),
					vmcs_region_pa);

				VMfailInvalid(&guest_context->flag_reg);
				break;
			}

			if (g_vcpus[vcpu_index] && (vmcs_region_pa == g_vcpus[vcpu_index]->vmxon_region))
			{
				HYPERPLATFORM_LOG_DEBUG(("kVmptrld: VMCS region %I64X same as VMXON region %I64X ! \r\n"),
					vmcs_region_pa, g_vcpus[vcpu_index]->vmxon_region);

				VMfailInvalid(&guest_context->flag_reg);
				break;
			}

			//VMCS id is not supported
			if (vmcs_region_va->revision_identifier != GetVMCSRevisionIdentifier())
			{
				HYPERPLATFORM_LOG_DEBUG(("VMPTRLD: VMCS revision identifier is not supported,  CPU supports identifier is : %x !"), GetVMCSRevisionIdentifier());
				VMfailInvalid(&guest_context->flag_reg);
				break;
			}

			PUCHAR			  vmcs_region_rw_va = (PUCHAR)ExAllocatePool(NonPagedPoolNx, PAGE_SIZE);
			ULONG64			  vmcs_region_rw_pa = UtilPaFromVa(vmcs_region_rw_va);

			RtlFillMemory(vmcs_region_rw_va, PAGE_SIZE, 0x0);

			USHORT vpid;
			VmRead16(VmcsField::kVirtualProcessorId, (ULONG_PTR)vmcs_region_va, &vpid);
			if (!vpid)
			{
				VmWrite16(VmcsField::kVirtualProcessorId, (ULONG_PTR)vmcs_region_rw_va, g_vpid);
			}
			else
			{
				VmWrite16(VmcsField::kVirtualProcessorId, (ULONG_PTR)vmcs_region_rw_va, vpid);
			}
			VmRead16(VmcsField::kVirtualProcessorId, (ULONG_PTR)vmcs_region_rw_va, &vpid);

			g_vcpus[vcpu_index]->current_vmcs = vmcs_region_rw_pa;		//vmcs02' physical address - we will control its structure in Vmread/Vmwrite 
			g_vcpus[vcpu_index]->guest_vmcs = vmcs_region_pa;		    //vmcs12' physical address - we will control its structure in Vmread/Vmwrite
			g_vcpus[vcpu_index]->kVirtualProcessorId = vpid;

			HYPERPLATFORM_LOG_DEBUG("VMPTRLD: Guest Instruction Pointer %I64X Guest Stack Pointer: %I64X  Guest VMCS PA: %I64X Guest VMCS VA : %I64X Run VMCS PA : %I64X Run VMCS VA : %I64X \r\n",
				InstructionPointer, StackPointer, vmcs_region_pa, vmcs_region_va, vmcs_region_rw_pa, vmcs_region_rw_va);

			HYPERPLATFORM_LOG_DEBUG("VMPTRLD: Run Successfully with VMCS_Region:  %I64X Total Vitrualized Core: %x  Current Cpu: %x in Cpu Group : %x  Number: %x \r\n",
				vmcs_region_pa, g_VM_Core_Count, g_vcpus[vcpu_index]->CpuNumber, procnumber.Group, procnumber.Number);

			HYPERPLATFORM_LOG_DEBUG("VMPTRLD: VCPU No.: %i Mode: %s Current VMCS : %I64X VMXON Region : %I64X  ",
				g_vcpus[vcpu_index]->CpuNumber, (g_vcpus[vcpu_index]->inVMX) ? "VMX" : "No VMX", g_vcpus[vcpu_index]->current_vmcs, g_vcpus[vcpu_index]->vmxon_region);

			VMSucceed(&guest_context->flag_reg);

		} while (FALSE);
	}

	//---------------------------------------------------------------------------------------------------------------------//
	VOID VmreadEmulate(GuestContext* guest_context)
	{

		do
		{
			PROCESSOR_NUMBER  procnumber = { 0 };
			ULONG			  vcpu_index = KeGetCurrentProcessorNumberEx(&procnumber);
			ULONG64			  vmcs12_pa = g_vcpus[vcpu_index]->guest_vmcs;
			ULONG64			  vmcs12_va = (ULONG64)UtilVaFromPa(vmcs12_pa);

			// if vCPU not run in VMX mode
			if (!g_vcpus[vcpu_index]->inVMX)
			{
				HYPERPLATFORM_LOG_DEBUG(("VMREAD: VMXON is required ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}
			//CR0.PE = 0;
			if (!IsGuestInProtectedMode())
			{
				HYPERPLATFORM_LOG_DEBUG(("VMREAD: Please running in Protected Mode ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}

			//If guest run in virtual-8086 mode
			//RFLAGS.VM = 1
			if (IsGuestInVirtual8086())
			{
				HYPERPLATFORM_LOG_DEBUG(("VMREAD: Guest is running in virtual-8086 mode ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}

			//If guest run in IA32-e 
			//kGuestIa32Efer.LMA = 1
			if (IsGuestInIA32eMode())
			{
				//If CS.L == 0 , means compability mode (32bit addressing), CS.L == 1 is 64bit mode , default operand is 32bit
				if (!IsGuestinCompatibliltyMode())
				{
					HYPERPLATFORM_LOG_DEBUG(("VMREAD: Guest is IA-32e mode but not in 64bit mode ! \r\n"));
					//#UD
					ThrowInvalidCodeException();
					break;
				}
			}

			///TODO: If in VMX non-root operation, should be VM Exit

			//Get Guest CPLvm
			if (GetGuestCPL() > 0)
			{
				HYPERPLATFORM_LOG_DEBUG(("VMREAD: Need running in Ring - 0 ! \r\n")); 	  //#gp
				ThrowGerneralFaultInterrupt();
				break;
			}

			VmcsField field;
			ULONG_PTR offset;
			ULONG_PTR value;
			BOOLEAN   RorM;
			ULONG_PTR regIndex;
			ULONG_PTR memAddress;

			field = DecodeVmwriteOrVmRead(guest_context->gp_regs, &offset, &value, &RorM, &regIndex, &memAddress);

			if (!is_vmcs_field_supported(field))
			{
				HYPERPLATFORM_LOG_DEBUG(("VMREAD: Need running in Ring - 0 ! \r\n")); 	  //#gp
				VMfailInvalid(&guest_context->flag_reg);
				break;
			}

			if ((ULONG64)vmcs12_va == 0xFFFFFFFFFFFFFFFF)
			{
				HYPERPLATFORM_LOG_DEBUG(("VMREAD: 0xFFFFFFFFFFFFFFFF		 ! \r\n")); 	  //#gp
				VMfailInvalid(&guest_context->flag_reg);
				break;
			}


			if (!g_vcpus[vcpu_index]->inRoot)
			{
				///TODO: Should INJECT vmexit to L1
				///	   And Handle it well
				break;
			}

			auto operand_size = VMCS_FIELD_WIDTH((int)field);


			if (RorM)
			{
				auto reg = VmmpSelectRegister((ULONG)regIndex, guest_context);
				if (operand_size == VMCS_FIELD_WIDTH_16BIT)
				{
					VmRead16(field, vmcs12_va, (PUSHORT)reg);
					HYPERPLATFORM_LOG_DEBUG("VMREAD16: field: %I64X base: %I64X Offset: %I64X Value: %I64X\r\n", field, vmcs12_va, offset, *(PUSHORT)reg);

				}
				if (operand_size == VMCS_FIELD_WIDTH_32BIT)
				{
					VmRead32(field, vmcs12_va, (PULONG32)reg);
					HYPERPLATFORM_LOG_DEBUG("VMREAD32: field: %I64X base: %I64X Offset: %I64X Value: %I64X\r\n", field, vmcs12_va, offset, *(PULONG32)reg);
				}
				if (operand_size == VMCS_FIELD_WIDTH_64BIT || operand_size == VMCS_FIELD_WIDTH_NATURAL_WIDTH)
				{
					VmRead64(field, vmcs12_va, (PULONG64)reg);
					HYPERPLATFORM_LOG_DEBUG("VMREAD64: field: %I64X base: %I64X Offset: %I64X Value: %I64X\r\n", field, vmcs12_va, offset, *(PULONG64)reg);
				}

			}
			else
			{
				if (operand_size == VMCS_FIELD_WIDTH_16BIT)
				{
					VmRead16(field, vmcs12_va, (PUSHORT)memAddress);
					HYPERPLATFORM_LOG_DEBUG("VMREAD16: field: %I64X base: %I64X Offset: %I64X Value: %I64X\r\n", field, vmcs12_va, offset, *(PUSHORT)memAddress);
				}
				if (operand_size == VMCS_FIELD_WIDTH_32BIT)
				{
					VmRead32(field, vmcs12_va, (PULONG32)memAddress);
					HYPERPLATFORM_LOG_DEBUG("VMREAD32: field: %I64X base: %I64X Offset: %I64X Value: %I64X\r\n", field, vmcs12_va, offset, *(PULONG32)memAddress);
				}
				if (operand_size == VMCS_FIELD_WIDTH_64BIT || operand_size == VMCS_FIELD_WIDTH_NATURAL_WIDTH)
				{
					VmRead64(field, vmcs12_va, (PULONG64)memAddress);
					HYPERPLATFORM_LOG_DEBUG("VMREAD64: field: %I64X base: %I64X Offset: %I64X Value: %I64X\r\n", field, vmcs12_va, offset, *(PULONG64)memAddress);
				}
			}
			VMSucceed(&guest_context->flag_reg);
		} while (FALSE);
	}

	//---------------------------------------------------------------------------------------------------------------------//

	VOID VmwriteEmulate(GuestContext* guest_context)
	{
		do
		{
			PROCESSOR_NUMBER  procnumber = { 0 };
			ULONG			  vcpu_index = KeGetCurrentProcessorNumberEx(&procnumber);
			ULONG64			  vmcs12_pa = (ULONG64)g_vcpus[vcpu_index]->guest_vmcs;
			ULONG64			  vmcs12_va = (ULONG64)UtilVaFromPa(vmcs12_pa);
			// if vCPU not run in VMX mode
			if (!g_vcpus[vcpu_index]->inVMX)
			{
				HYPERPLATFORM_LOG_DEBUG(("VMWRITE: VMXON is required ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}
			//CR0.PE = 0;
			if (!IsGuestInProtectedMode())
			{
				HYPERPLATFORM_LOG_DEBUG(("VMWRITE: Please running in Protected Mode ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}

			//If guest run in virtual-8086 mode
			//RFLAGS.VM = 1
			if (IsGuestInVirtual8086())
			{
				HYPERPLATFORM_LOG_DEBUG(("VMWRITE: Guest is running in virtual-8086 mode ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}

			//If guest run in IA32-e 
			//kGuestIa32Efer.LMA = 1
			if (IsGuestInIA32eMode())
			{
				//If CS.L == 0 , means compability mode (32bit addressing), CS.L == 1 is 64bit mode , default operand is 32bit
				if (!IsGuestinCompatibliltyMode())
				{
					HYPERPLATFORM_LOG_DEBUG(("VMWRITE: Guest is IA-32e mode but not in 64bit mode ! \r\n"));
					//#UD
					ThrowInvalidCodeException();
					break;
				}
			}

			///TODO: If in VMX non-root operation, should be VM Exit

			//Get Guest CPL
			if (GetGuestCPL() > 0)
			{
				HYPERPLATFORM_LOG_DEBUG(("VMWRITE: Need running in Ring - 0 ! \r\n")); 	  //#gp
				ThrowGerneralFaultInterrupt();
				break;
			}

			VmcsField field;
			ULONG_PTR offset;
			ULONG_PTR Value;
			BOOLEAN   RorM;

			field = DecodeVmwriteOrVmRead(guest_context->gp_regs, &offset, &Value, &RorM);

			if (!is_vmcs_field_supported(field))
			{
				HYPERPLATFORM_LOG_DEBUG("VMWRITE: IS NOT SUPPORT %X ! \r\n", field); 	  //#gp
				VMfailInvalid(&guest_context->flag_reg);
				break;
			}

			if (!g_vcpus[vcpu_index]->inRoot)
			{
				///TODO: Should INJECT vmexit to L1
				///	   And Handle it well
				break;
			}
			auto operand_size = VMCS_FIELD_WIDTH((int)field);
			if (operand_size == VMCS_FIELD_WIDTH_16BIT)
			{
				VmWrite16(field, vmcs12_va, Value);
				HYPERPLATFORM_LOG_DEBUG("VMWRITE: field: %I64X base: %I64X Offset: %I64X Value: %I64X  \r\n", field, vmcs12_va, offset, (USHORT)Value);
			}

			if (operand_size == VMCS_FIELD_WIDTH_32BIT)
			{
				VmWrite32(field, vmcs12_va, Value);
				HYPERPLATFORM_LOG_DEBUG("VMWRITE: field: %I64X base: %I64X Offset: %I64X Value: %I64X\r\n", field, vmcs12_va, offset, (ULONG32)Value);
			}
			if (operand_size == VMCS_FIELD_WIDTH_64BIT || operand_size == VMCS_FIELD_WIDTH_NATURAL_WIDTH)
			{
				VmWrite64(field, vmcs12_va, Value);
				HYPERPLATFORM_LOG_DEBUG("VMWRITE: field: %I64X base: %I64X Offset: %I64X Value: %I64X\r\n", field, vmcs12_va, offset, (ULONG64)Value);
			}

			VMSucceed(&guest_context->flag_reg);
		} while (FALSE);
	}
	/*
	64 bit Control field is not used

	// below not used
	// kVmExitMsrStoreAddr = 0x00002006,
	// kVmExitMsrStoreAddrHigh = 0x00002007,
	// kVmExitMsrLoadAddr = 0x00002008,
	// kVmExitMsrLoadAddrHigh = 0x00002009,
	// kVmEntryMsrLoadAddr = 0x0000200a,
	// kVmEntryMsrLoadAddrHigh = 0x0000200b,
	// kExecutiveVmcsPointer = 0x0000200c,
	// kExecutiveVmcsPointerHigh = 0x0000200d,


	// below not used
	kTscOffset = 0x00002010,
	kTscOffsetHigh = 0x00002011,
	kVirtualApicPageAddr = 0x00002012,
	kVirtualApicPageAddrHigh = 0x00002013,
	kApicAccessAddrHigh = 0x00002015,
	kPostedInterruptDescAddr  =  0x00002016,
	kPostedInterruptDescAddrHigh = 0x00002017,

	// below not used
	kVmreadBitmapAddress = 0x00002026,
	kVmreadBitmapAddressHigh = 0x00002027,
	kVmwriteBitmapAddress = 0x00002028,
	kVmwriteBitmapAddressHigh = 0x00002029,
	kVirtualizationExceptionInfoAddress = 0x0000202a,
	kVirtualizationExceptionInfoAddressHigh = 0x0000202b,
	kXssExitingBitmap = 0x0000202c,
	kXssExitingBitmapHigh = 0x0000202d,
	*/

	/*----------------------------------------------------------------------------------------------------

	VMCS02 Structure
	--------------------------------------
	16/32/64/Natrual Guest state field :  VMCS12
	16/32/64/Natrual Host  state field :  VMCS01
	16/32/64/Natrual Control field	   :  VMCS01+VMCS12

	----------------------------------------------------------------------------------------------------*/


	//---------------------------------------------------------------------------------------------------------------------//

	VOID VmlaunchEmulate(GuestContext* guest_context)
	{
		PROCESSOR_NUMBER  procnumber = { 0 };
		ULONG			  vcpu_index = KeGetCurrentProcessorNumberEx(&procnumber);
		VmxStatus		  status;
		do {
			//not in vmx mode
			if (!g_vcpus[vcpu_index]->inVMX)
			{
				HYPERPLATFORM_LOG_DEBUG(("VMWRITE: VMXON is required ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}

			//CR0.PE = 0;
			if (!IsGuestInProtectedMode())
			{
				HYPERPLATFORM_LOG_DEBUG(("VMWRITE: Please running in Protected Mode ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}

			//If guest run in virtual-8086 mode
			//RFLAGS.VM = 1
			if (IsGuestInVirtual8086())
			{
				HYPERPLATFORM_LOG_DEBUG(("VMWRITE: Guest is running in virtual-8086 mode ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}

			//If guest run in IA32-e
			//kGuestIa32Efer.LMA = 1
			if (IsGuestInIA32eMode())
			{
				//If CS.L == 0 , means compability mode (32bit addressing), CS.L == 1 is 64bit mode , default operand is 32bit
				if (!IsGuestinCompatibliltyMode())
				{
					HYPERPLATFORM_LOG_DEBUG(("VMWRITE: Guest is IA-32e mode but not in 64bit mode ! \r\n"));
					//#UD
					ThrowInvalidCodeException();
					break;
				}
			}
			//Get Guest CPL
			if (GetGuestCPL() > 0)
			{
				HYPERPLATFORM_LOG_DEBUG(("VMLAUNCH: Need running in Ring - 0 ! \r\n")); 	  //#gp
				ThrowGerneralFaultInterrupt();
				break;
			}


			if (!g_vcpus[vcpu_index]->inRoot)
			{
				///TODO: Should INJECT vmexit to L1
				///	   And Handle it well
				break;
			}
			//vmcs02
			auto vmcs02_pa = g_vcpus[vcpu_index]->current_vmcs;
			auto vmcs02_va = (ULONG64)UtilVaFromPa(vmcs02_pa);
			if (vmcs02_pa == 0xFFFFFFFFFFFFFFFF)
			{
				HYPERPLATFORM_LOG_DEBUG(("VMLAUNCH: VMCS still not loaded ! \r\n"));
				VMfailInvalid(&guest_context->flag_reg);
				break;
			}

			///1. Check Setting of VMX Controls and Host State area;
			///2. Attempt to load guest state and PDPTRs as appropriate
			///3. Attempt to load MSRs from VM-Entry MSR load area;
			///4. Set VMCS to "launched"
			///5. VM Entry success


			//Guest passed it to us, and read/write it  VMCS 1-2 
			auto  vmcs12_pa = g_vcpus[vcpu_index]->guest_vmcs;
			auto  vmcs12_va = (ULONG64)UtilVaFromPa(vmcs12_pa);
			
			// Write a VMCS revision identifier
			const Ia32VmxBasicMsr vmx_basic_msr = { UtilReadMsr64(Msr::kIa32VmxBasic) };
				RtlFillMemory((PVOID)vmcs02_va, 0, PAGE_SIZE);
			VmControlStructure* ptr = (VmControlStructure*)vmcs02_va;
			ptr->revision_identifier = vmx_basic_msr.fields.revision_identifier;
			
			ULONG64 vmcs01_rsp = UtilVmRead64(VmcsField::kHostRsp);
			ULONG64 vmcs01_rip = UtilVmRead64(VmcsField::kHostRip);
			
			/*
				Mix vmcs control field
			*/
			MixControlFieldWithVmcs01AndVmcs12(vmcs12_va, vmcs02_pa, TRUE);

			//Read VMCS12 Guest's field to VMCS02
			FillGuestFieldFromVMCS12(vmcs12_va);

			/*
			VM Host state field Start
			*/ 
			FillHostStateFieldByPhysicalCpu(vmcs01_rip, vmcs01_rsp);

			/*
			Host state field end
			*/

			//--------------------------------------------------------------------------------------//

			ULONG64 rip, rsp, rflags;
			//Get VMCS 1-2 Guest Rip & Rsp , means where is it trapped by 
			//Get Vmlaunch return address from VMCS12 (it supposed provided by L1 already), there's no way to verify it 
			VmRead64(VmcsField::kGuestRip, vmcs12_va, &rip);
			VmRead64(VmcsField::kGuestRsp, vmcs12_va, &rsp);
			VmRead64(VmcsField::kGuestRflags, vmcs12_va, &rflags);

			UtilVmWrite(VmcsField::kGuestRsp, rsp);
			UtilVmWrite(VmcsField::kGuestRip, rip);
			UtilVmWrite(VmcsField::kGuestRflags, rflags);
			PrintVMCS();

			if (guest_context->irql < DISPATCH_LEVEL)
			{
				KeLowerIrql(guest_context->irql);
			}

			if (VmxStatus::kOk != (status = static_cast<VmxStatus>(__vmx_vmlaunch())))
			{
				VmxInstructionError error2 = static_cast<VmxInstructionError>(UtilVmRead(VmcsField::kVmInstructionError));
				HYPERPLATFORM_LOG_DEBUG("Error vmclear error code :%x , %x ", status, error2);
				HYPERPLATFORM_COMMON_DBG_BREAK();
			}

			HYPERPLATFORM_LOG_DEBUG("Error vmclear error code :%x , %x ", 0, 0);

			return;
		} while (FALSE);

	}




	VOID VmresumeEmulate(GuestContext* guest_context)
	{
		do
		{
			PROCESSOR_NUMBER  procnumber = { 0 };
			ULONG			  vcpu_index = KeGetCurrentProcessorNumberEx(&procnumber);

			HYPERPLATFORM_LOG_DEBUG("----Start Emulate VMRESUME---");
			//not in vmx mode
			if (!g_vcpus[vcpu_index]->inVMX)
			{
				HYPERPLATFORM_LOG_DEBUG(("VMWRITE: VMXON is required ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}

			//CR0.PE = 0;
			if (!IsGuestInProtectedMode())
			{
				HYPERPLATFORM_LOG_DEBUG(("VMWRITE: Please running in Protected Mode ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}

			//If guest run in virtual-8086 mode
			//RFLAGS.VM = 1
			if (IsGuestInVirtual8086())
			{
				HYPERPLATFORM_LOG_DEBUG(("VMWRITE: Guest is running in virtual-8086 mode ! \r\n"));
				//#UD
				ThrowInvalidCodeException();
				break;
			}

			//If guest run in IA32-e
			//kGuestIa32Efer.LMA = 1
			if (IsGuestInIA32eMode())
			{
				//If CS.L == 0 , means compability mode (32bit addressing), CS.L == 1 is 64bit mode , default operand is 32bit
				if (!IsGuestinCompatibliltyMode())
				{
					HYPERPLATFORM_LOG_DEBUG(("VMWRITE: Guest is IA-32e mode but not in 64bit mode ! \r\n"));
					//#UD
					ThrowInvalidCodeException();
					break;
				}
			}
			//Get Guest CPL
			if (GetGuestCPL() > 0)
			{
				HYPERPLATFORM_LOG_DEBUG(("VMLAUNCH: Need running in Ring - 0 ! \r\n")); 	  //#gp
				ThrowGerneralFaultInterrupt();
				break;
			}

			//We created it when guest VMPTRLD VMCS12 , it called VMCS02 , what L2 actually running on
			auto vmcs02_pa = g_vcpus[vcpu_index]->current_vmcs;
			auto vmcs02_va = (ULONG64)UtilVaFromPa(vmcs02_pa);

			//Guest passed it to us, and read/write it  VMCS 1-2 
			auto  vmcs12_pa = g_vcpus[vcpu_index]->guest_vmcs;
			auto  vmcs12_va = (ULONG64)UtilVaFromPa(vmcs12_pa);

			if (vmcs02_pa == 0xFFFFFFFFFFFFFFFF)
			{
				HYPERPLATFORM_LOG_DEBUG(("VMLAUNCH: VMCS still not loaded ! \r\n"));
				VMfailInvalid(&guest_context->flag_reg);
				break;
			}

			///1. Check Setting of VMX Controls and Host State area;
			///2. Attempt to load guest state and PDPTRs as appropriate
			///3. Attempt to load MSRs from VM-Entry MSR load area;
			///4. Set VMCS to "launched"
			///5. VM Entry success

			
			// Write a VMCS revision identifier
			const Ia32VmxBasicMsr vmx_basic_msr = { UtilReadMsr64(Msr::kIa32VmxBasic) };
			ULONG64 vmcs01_rsp = UtilVmRead64(VmcsField::kHostRsp);
			ULONG64 vmcs01_rip = UtilVmRead64(VmcsField::kHostRip); 

			VmControlStructure* ptr = (VmControlStructure*)vmcs02_va;
			ptr->revision_identifier = vmx_basic_msr.fields.revision_identifier;

			HYPERPLATFORM_LOG_DEBUG("current_vmcs: %I64X", vmcs12_va);
		
			//Mixing AND checking Control Field 
			//Current VMCS is VMCS01
			MixControlFieldWithVmcs01AndVmcs12(vmcs12_va, vmcs02_pa, FALSE);
			//Current VMCS is VMCS02 


			/*
			VM Guest state field Start
			*/
			
			FillGuestFieldFromVMCS12(vmcs12_va);
		
			/*
			VM Guest state field End
			*/


			/*
			VM Host state field Start
			*/
			FillHostStateFieldByPhysicalCpu(vmcs01_rip, vmcs01_rsp);
			/*
			VM Host state field End
			*/

			//--------------------------------------------------------------------------------------//

			/*
			*		After L1 handles any VM Exit and should be executes VMRESUME for back L2
			*		But this time trapped by VMCS01 and We can't get any VM-Exit information
			*       from it. So we need to read from VMCS12 and return from here immediately.
			*		We saved the vmcs02 GuestRip into VMCS12 our VMExit Handler because when
			*		L1 was executing VMRESUME(We injected VMExit to it), and it is running on
			*		VMCS01, we can't and shouldn't change it.
			*		See: VmmVmExitHandler
			*/

			//--------------------------------------------------------------------------------------//

			// Explicitly write once again. It is important and will be cause the system hang.
			ULONG64   rip = 0;
			VmRead64(VmcsField::kGuestRip, vmcs12_va, &rip);
			HYPERPLATFORM_LOG_DEBUG("VMCS12 Guest rip : %I64x guest_context->irql: %i", rip, guest_context->irql);

			ULONG64   rsp = 0;
			VmRead64(VmcsField::kGuestRsp, vmcs12_va, &rsp);
			HYPERPLATFORM_LOG_DEBUG("VMCS12 Guest rsp : %I64x ", rsp);
			ULONG64   rflags = 0;
			VmRead64(VmcsField::kGuestRflags, vmcs12_va, &rflags);
			HYPERPLATFORM_LOG_DEBUG("VMCS12 Guest rsp : %I64x ", rflags);


			UtilVmWrite(VmcsField::kGuestRip, rip);
			UtilVmWrite(VmcsField::kGuestRsp, rsp);
			UtilVmWrite(VmcsField::kGuestRflags, rflags);

			PrintVMCS();
			
			HYPERPLATFORM_LOG_DEBUG("VMCS02: kGuestRip :%I64x , kGuestRsp %I64x ", UtilVmRead(VmcsField::kGuestRip), UtilVmRead(VmcsField::kGuestRsp));
			HYPERPLATFORM_LOG_DEBUG("VMCS02: kHostRip :%I64x  , kHostRsp  %I64x ", UtilVmRead(VmcsField::kHostRip), UtilVmRead(VmcsField::kHostRsp));

			if (guest_context->irql < DISPATCH_LEVEL)
			{
				KeLowerIrql(guest_context->irql);
			}
			 
			HYPERPLATFORM_COMMON_DBG_BREAK();
		} while (FALSE);
	}


	//----------------------------------------------------------------------------------------------------------------//

	_Use_decl_annotations_ static void VmmpHandleVmx(GuestContext *guest_context, VmxExitReason VMXReasons) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();


		//	Assumption:  If and only If trapped by L1 only,
		//    TODO	  :  Trapped by L2 handler

		switch (VMXReasons)
		{
		case VmxExitReason::kVmon:
		{
			VmxonEmulate(guest_context);
		}
		break;

		/*
		Simpily clear VMCS02 and VMCS12
		*/
		case VmxExitReason::kVmclear:
		{
			VmclearEmulate(guest_context);
		}
		break;

		/*
		Load VMCS02 into physical cpu , And perform some check on VMCS12
		*/
		case VmxExitReason::kVmptrld:
		{
			VmptrldEmulate(guest_context);
		}
		break;

		/// TODO:
		case VmxExitReason::kVmoff:
		{
			VMSucceed(&guest_context->flag_reg);
			HYPERPLATFORM_LOG_DEBUG("nested vmxoff \r\n");
		}
		break;
		/*
		ReadWrite on VMCS12 and VMCS02
		*/
		case VmxExitReason::kVmwrite:
		{
			VmwriteEmulate(guest_context);
		}
		break;
		/*
		ReadWrite on VMCS12 and VMCS02
		*/
		case VmxExitReason::kVmread:
		{
			VmreadEmulate(guest_context);
		}
		break;

		case VmxExitReason::kVmlaunch:
		{
			VmlaunchEmulate(guest_context);
		}
		break;

		case VmxExitReason::kVmresume:
		{
			//In VmresumeEmulate will modify Guest Rip so no need to execute anymore after it returns.
			//And should not execute anymore.

			//Vmresume Emulation :
			//- Fill Guest, Host, Control field state in VMCS02
			//- Read GuestRIP from VMCS02 as it is trapped by L2 if Vmresume is trapped by L1
			//- So we need to help L1 to resume to L2
			//- We saved the vmcs02 GuestRip into VMCS12 our VMExit Handler, 
			//- because when L1 is executing VMRESUME, it is running on VMCS01
			VmresumeEmulate(guest_context);
			return;
		}
		break;

		case VmxExitReason::kVmxPreemptionTime:
		{
			VMSucceed(&guest_context->flag_reg);
			HYPERPLATFORM_LOG_DEBUG("nested kVmwrite \r\n");
		}
		break;

		case VmxExitReason::kInvept:
		default:
		{
			VMSucceed(&guest_context->flag_reg);
			HYPERPLATFORM_LOG_DEBUG("NOT SURE VMX \r\n");
		}
		break;
		}

		UtilVmWrite(VmcsField::kGuestRflags, guest_context->flag_reg.all);
		VmmpAdjustGuestInstructionPointer(guest_context->ip);

	}
	// VMCALL
	_Use_decl_annotations_ static void VmmpHandleVmCall(
		GuestContext *guest_context)
	{
		// VMCALL for Sushi expects that cx holds a command number, and dx holds an
		// address of a context parameter optionally
		const auto hypercall_number = static_cast<HypercallNumber>(guest_context->gp_regs->cx);
		const auto context = reinterpret_cast<void *>(guest_context->gp_regs->dx);

		if (hypercall_number == HypercallNumber::kTerminateVmm)
		{
			// Unloading requested
			HYPERPLATFORM_COMMON_DBG_BREAK();

			// The processor sets ffff to limits of IDT and GDT when VM-exit occurred.
			// It is not correct value but fine to ignore since vmresume loads correct
			// values from VMCS. But here, we are going to skip vmresume and simply
			// return to where VMCALL is executed. It results in keeping those broken
			// values and ends up with bug check 109, so we should fix them manually.
			const auto gdt_limit = UtilVmRead(VmcsField::kGuestGdtrLimit);
			const auto gdt_base = UtilVmRead(VmcsField::kGuestGdtrBase);
			const auto idt_limit = UtilVmRead(VmcsField::kGuestIdtrLimit);
			const auto idt_base = UtilVmRead(VmcsField::kGuestIdtrBase);
			Gdtr gdtr = { static_cast<USHORT>(gdt_limit), gdt_base };
			Idtr idtr = { static_cast<USHORT>(idt_limit), idt_base };
			__lgdt(&gdtr);
			__lidt(&idtr);

			// Store an address of the management structure to the context parameter
			const auto result_ptr = reinterpret_cast<ProcessorData **>(context);
			*result_ptr = guest_context->stack->processor_data;
			HYPERPLATFORM_LOG_DEBUG_SAFE("Context at %p %p", context,
				guest_context->stack->processor_data);

			// Set rip to the next instruction of VMCALL
			const auto exit_instruction_length =
				UtilVmRead(VmcsField::kVmExitInstructionLen);
			const auto return_address = guest_context->ip + exit_instruction_length;

			// Since rflags is overwritten after VMXOFF, we should manually indicates
			// that VMCALL was successful by clearing those flags.
			guest_context->flag_reg.fields.cf = false;
			guest_context->flag_reg.fields.zf = false;

			// Set registers used after VMXOFF to recover the context. Volatile
			// registers must be used because those changes are reflected to the
			// guest's context after VMXOFF.
			guest_context->gp_regs->cx = return_address;
			guest_context->gp_regs->dx = guest_context->gp_regs->sp;
			guest_context->gp_regs->ax = guest_context->flag_reg.all;
			guest_context->vm_continue = false;

		}
		else if (hypercall_number == HypercallNumber::kShEnablePageShadowing)
		{

			ShEnablePageShadowing(
				guest_context->stack->processor_data->ept_data,
				guest_context->stack->processor_data->shared_data->shared_sh_data);

			VmmpAdjustGuestInstructionPointer(guest_context->ip);

			// Indicates successful VMCALL
			guest_context->flag_reg.fields.cf = false;
			guest_context->flag_reg.fields.zf = false;
			UtilVmWrite(VmcsField::kGuestRflags, guest_context->flag_reg.all);

		}
		else if (hypercall_number == HypercallNumber::kShDisablePageShadowing)
		{
			ShVmCallDisablePageShadowing(
				guest_context->stack->processor_data->ept_data,
				guest_context->stack->processor_data->shared_data->shared_sh_data);

			VmmpAdjustGuestInstructionPointer(guest_context->ip);
			// Indicates successful VMCALL
			guest_context->flag_reg.fields.cf = false;
			guest_context->flag_reg.fields.zf = false;
			UtilVmWrite(VmcsField::kGuestRflags, guest_context->flag_reg.all);

		}

		else {
			// Unsupported hypercall. Handle like other VMX instructions
			VmmpHandleVmx(guest_context, VmxExitReason::kVmcall);
		}
	}

	// INVD
	_Use_decl_annotations_ static void VmmpHandleInvalidateInternalCaches(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		AsmInvalidateInternalCaches();
		VmmpAdjustGuestInstructionPointer(guest_context->ip);
	}

	// INVLPG
	_Use_decl_annotations_ static void VmmpHandleInvalidateTLBEntry(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		const auto invalidate_address =
			reinterpret_cast<void *>(UtilVmRead(VmcsField::kExitQualification));
		__invlpg(invalidate_address);
		VmmpAdjustGuestInstructionPointer(guest_context->ip);
	}

	// EXIT_REASON_EPT_VIOLATION
	_Use_decl_annotations_ static void VmmpHandleEptViolation(
		GuestContext *guest_context) {
		HYPERPLATFORM_PERFORMANCE_MEASURE_THIS_SCOPE();
		auto processor_data = guest_context->stack->processor_data;

		//DbgPrint("EPT Violation Current CR3: 0x%08x Guest CR3: 0x%08x", __readcr3(), UtilVmRead64(VmcsField::kGuestCr3));

		EptHandleEptViolation(
			processor_data->ept_data,
			processor_data->sh_data,
			processor_data->shared_data->shared_sh_data);
	}

	// EXIT_REASON_EPT_MISCONFIG
	_Use_decl_annotations_ static void VmmpHandleEptMisconfig(GuestContext *guest_context) 
	{
		UNREFERENCED_PARAMETER(guest_context);
		const auto fault_address = UtilVmRead(VmcsField::kGuestPhysicalAddress);
		const auto ept_pt_entry = EptGetEptPtEntry(guest_context->stack->processor_data->ept_data, fault_address);
		HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kEptMisconfigVmExit,fault_address,reinterpret_cast<ULONG_PTR>(ept_pt_entry), 0);
	}

	// Selects a register to be used based on the index
	_Use_decl_annotations_ static ULONG_PTR *VmmpSelectRegister(
		ULONG index, GuestContext *guest_context) {
		ULONG_PTR *register_used = nullptr;
		// clang-format off
		switch (index) {
		case 0: register_used = &guest_context->gp_regs->ax; break;
		case 1: register_used = &guest_context->gp_regs->cx; break;
		case 2: register_used = &guest_context->gp_regs->dx; break;
		case 3: register_used = &guest_context->gp_regs->bx; break;
		case 4: register_used = &guest_context->gp_regs->sp; break;
		case 5: register_used = &guest_context->gp_regs->bp; break;
		case 6: register_used = &guest_context->gp_regs->si; break;
		case 7: register_used = &guest_context->gp_regs->di; break;
#if defined(_AMD64_)
		case 8: register_used = &guest_context->gp_regs->r8; break;
		case 9: register_used = &guest_context->gp_regs->r9; break;
		case 10: register_used = &guest_context->gp_regs->r10; break;
		case 11: register_used = &guest_context->gp_regs->r11; break;
		case 12: register_used = &guest_context->gp_regs->r12; break;
		case 13: register_used = &guest_context->gp_regs->r13; break;
		case 14: register_used = &guest_context->gp_regs->r14; break;
		case 15: register_used = &guest_context->gp_regs->r15; break;
#endif
		default: HYPERPLATFORM_COMMON_DBG_BREAK(); break;
		}
		// clang-format on
		return register_used;
	}

	// Dumps guest's selectors
	/*_Use_decl_annotations_*/ static void VmmpDumpGuestSelectors() {
		HYPERPLATFORM_LOG_DEBUG_SAFE(
			"es %04x %p %08x %08x", UtilVmRead(VmcsField::kGuestEsSelector),
			UtilVmRead(VmcsField::kGuestEsBase), UtilVmRead(VmcsField::kGuestEsLimit),
			UtilVmRead(VmcsField::kGuestEsArBytes));
		HYPERPLATFORM_LOG_DEBUG_SAFE(
			"cs %04x %p %08x %08x", UtilVmRead(VmcsField::kGuestCsSelector),
			UtilVmRead(VmcsField::kGuestCsBase), UtilVmRead(VmcsField::kGuestCsLimit),
			UtilVmRead(VmcsField::kGuestCsArBytes));
		HYPERPLATFORM_LOG_DEBUG_SAFE(
			"ss %04x %p %08x %08x", UtilVmRead(VmcsField::kGuestSsSelector),
			UtilVmRead(VmcsField::kGuestSsBase), UtilVmRead(VmcsField::kGuestSsLimit),
			UtilVmRead(VmcsField::kGuestSsArBytes));
		HYPERPLATFORM_LOG_DEBUG_SAFE(
			"ds %04x %p %08x %08x", UtilVmRead(VmcsField::kGuestDsSelector),
			UtilVmRead(VmcsField::kGuestDsBase), UtilVmRead(VmcsField::kGuestDsLimit),
			UtilVmRead(VmcsField::kGuestDsArBytes));
		HYPERPLATFORM_LOG_DEBUG_SAFE(
			"fs %04x %p %08x %08x", UtilVmRead(VmcsField::kGuestFsSelector),
			UtilVmRead(VmcsField::kGuestFsBase), UtilVmRead(VmcsField::kGuestFsLimit),
			UtilVmRead(VmcsField::kGuestFsArBytes));
		HYPERPLATFORM_LOG_DEBUG_SAFE(
			"gs %04x %p %08x %08x", UtilVmRead(VmcsField::kGuestGsSelector),
			UtilVmRead(VmcsField::kGuestGsBase), UtilVmRead(VmcsField::kGuestGsLimit),
			UtilVmRead(VmcsField::kGuestGsArBytes));
		HYPERPLATFORM_LOG_DEBUG_SAFE("ld %04x %p %08x %08x",
			UtilVmRead(VmcsField::kGuestLdtrSelector),
			UtilVmRead(VmcsField::kGuestLdtrBase),
			UtilVmRead(VmcsField::kGuestLdtrLimit),
			UtilVmRead(VmcsField::kGuestLdtrArBytes));
		HYPERPLATFORM_LOG_DEBUG_SAFE(
			"tr %04x %p %08x %08x", UtilVmRead(VmcsField::kGuestTrSelector),
			UtilVmRead(VmcsField::kGuestTrBase), UtilVmRead(VmcsField::kGuestTrLimit),
			UtilVmRead(VmcsField::kGuestTrArBytes));
	}

	// Sets rip to the next instruction
	_Use_decl_annotations_ static void VmmpAdjustGuestInstructionPointer(
		ULONG_PTR guest_ip) {
		const auto exit_instruction_length =
			UtilVmRead(VmcsField::kVmExitInstructionLen);
		UtilVmWrite(VmcsField::kGuestRip, guest_ip + exit_instruction_length);
	}

	// Handle VMRESUME or VMXOFF failure. Fatal error.
	_Use_decl_annotations_ void __stdcall VmmVmxFailureHandler(
		AllRegisters *all_regs) {
		const auto vmx_error = (all_regs->flags.fields.zf)
			? UtilVmRead(VmcsField::kVmInstructionError)
			: 0;
		HYPERPLATFORM_COMMON_BUG_CHECK(
			HyperPlatformBugCheck::kCriticalVmxInstructionFailure, vmx_error, 0, 0);
	}

}  // extern "C"
