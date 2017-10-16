#include "stdafx.h"

namespace ddk::wsk
{
	//΢���Ĵ��븴�ƹ���
	WSK_REGISTRATION         g_WskRegistration;
	WSK_PROVIDER_NPI         g_WskProvider;
	WSK_CLIENT_DISPATCH      g_WskDispatch = { MAKE_WSK_VERSION(1,0), 0, NULL };
	LONG g_WskStartCount = 0;
	enum
	{
		DEINITIALIZED,
		DEINITIALIZING,
		INITIALIZING,
		INITIALIZED
	};

	LONG     g_SocketsState = DEINITIALIZED;
	IO_COMPLETION_ROUTINE CompletionRoutine;

	_Use_decl_annotations_
	NTSTATUS
		CompletionRoutine(
			__in PDEVICE_OBJECT 		DeviceObject,
			__in PIRP                   Irp,
			__in PVOID                  Context
		)
	{
		ASSERT(Context);

		UNREFERENCED_PARAMETER(Irp);
		UNREFERENCED_PARAMETER(DeviceObject);
		auto CompletionEvent = reinterpret_cast<PKEVENT>(Context);
		KeSetEvent(CompletionEvent, IO_NO_INCREMENT, FALSE);
		return STATUS_MORE_PROCESSING_REQUIRED;
	}


	NTSTATUS
		InitWskData(
			__out PIRP*             pIrp,
			__out PKEVENT   CompletionEvent
		)
	{
		ASSERT(pIrp);
		ASSERT(CompletionEvent);

		*pIrp = IoAllocateIrp(1, FALSE);
		if (!*pIrp) {
			LOG_DEBUG("InitWskData(): IoAllocateIrp() failed\n");
			return STATUS_INSUFFICIENT_RESOURCES;
		}

		KeInitializeEvent(CompletionEvent, SynchronizationEvent, FALSE);
		IoSetCompletionRoutine(*pIrp, CompletionRoutine, CompletionEvent, TRUE, TRUE, TRUE);
		return STATUS_SUCCESS;
	}


	NTSTATUS
		InitWskBuffer(
			__in  PVOID             Buffer,
			__in  ULONG             BufferSize,
			__out PWSK_BUF  WskBuffer
		)
	{
		NTSTATUS Status = STATUS_SUCCESS;

		ASSERT(Buffer);
		ASSERT(BufferSize);
		ASSERT(WskBuffer);

		WskBuffer->Offset = 0;
		WskBuffer->Length = BufferSize;

		WskBuffer->Mdl = IoAllocateMdl(Buffer, BufferSize, FALSE, FALSE, NULL);
		if (!WskBuffer->Mdl) {
			LOG_DEBUG("InitWskBuffer(): IoAllocateMdl() failed\n");
			return STATUS_INSUFFICIENT_RESOURCES;
		}

		__try {
			MmProbeAndLockPages(WskBuffer->Mdl, KernelMode, IoWriteAccess);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			LOG_DEBUG("InitWskBuffer(): MmProbeAndLockPages(%p) failed\n", Buffer);
			IoFreeMdl(WskBuffer->Mdl);
			Status = STATUS_ACCESS_VIOLATION;
		}

		return Status;
	}


	VOID
		FreeWskBuffer(
			__in PWSK_BUF WskBuffer
		)
	{
		ASSERT(WskBuffer);


		MmUnlockPages(WskBuffer->Mdl);
		IoFreeMdl(WskBuffer->Mdl);
	}

	//
	// Library initialization routine
	//

	NTSTATUS NTAPI WSKStartup()
	{
		WSK_CLIENT_NPI  WskClient = { 0 };
		NTSTATUS                Status = STATUS_UNSUCCESSFUL;

		InterlockedIncrement(&g_WskStartCount);

		if (InterlockedCompareExchange(&g_SocketsState, INITIALIZING, DEINITIALIZED) != DEINITIALIZED)
			return STATUS_ALREADY_REGISTERED;

		WskClient.ClientContext = NULL;
		WskClient.Dispatch = &g_WskDispatch;

		Status = WskRegister(&WskClient, &g_WskRegistration);
		if (!NT_SUCCESS(Status)) {
			LOG_DEBUG("WskRegister() failed with status 0x%08X\n", Status);
			InterlockedExchange(&g_SocketsState, DEINITIALIZED);
			return Status;
		}

		Status = WskCaptureProviderNPI(&g_WskRegistration, WSK_NO_WAIT, &g_WskProvider);
		if (!NT_SUCCESS(Status)) {
			LOG_DEBUG("WskCaptureProviderNPI() failed with status 0x%08X\n", Status);
			WskDeregister(&g_WskRegistration);
			InterlockedExchange(&g_SocketsState, DEINITIALIZED);
			return Status;
		}

		InterlockedExchange(&g_SocketsState, INITIALIZED);
		return STATUS_SUCCESS;
	}

	//
	// Library deinitialization routine
	//

	VOID NTAPI WSKCleanup()
	{
		InterlockedDecrement(&g_WskStartCount);

		if (g_WskStartCount != 0)
		{
			return;
		}

		if (InterlockedCompareExchange(&g_SocketsState, INITIALIZED, DEINITIALIZING) != INITIALIZED)
			return;

		WskReleaseProviderNPI(&g_WskRegistration);
		WskDeregister(&g_WskRegistration);

		InterlockedExchange(&g_SocketsState, DEINITIALIZED);
	}



	PWSK_SOCKET
		NTAPI
		CreateSocket(
			__in ADDRESS_FAMILY AddressFamily,
			__in USHORT                 SocketType,
			__in ULONG                  Protocol,
			__in ULONG                  Flags
		)
	{
		KEVENT                  CompletionEvent = { 0 };
		PIRP                    Irp = NULL;
		PWSK_SOCKET             WskSocket = NULL;
		NTSTATUS                Status = STATUS_UNSUCCESSFUL;

		if (g_SocketsState != INITIALIZED)
			return NULL;

		Status = InitWskData(&Irp, &CompletionEvent);
		if (!NT_SUCCESS(Status)) {
			LOG_DEBUG("CreateSocket(): InitWskData() failed with status 0x%08X\n", Status);
			return NULL;
		}

		Status = g_WskProvider.Dispatch->WskSocket(
			g_WskProvider.Client,
			AddressFamily,
			SocketType,
			Protocol,
			Flags,
			NULL,
			NULL,
			NULL,
			NULL,
			NULL,
			Irp);
		if (Status == STATUS_PENDING) {
			KeWaitForSingleObject(&CompletionEvent, Executive, KernelMode, FALSE, NULL);
			Status = Irp->IoStatus.Status;
		}

		WskSocket = NT_SUCCESS(Status) ? (PWSK_SOCKET)Irp->IoStatus.Information : NULL;

		IoFreeIrp(Irp);
		return (PWSK_SOCKET)WskSocket;
	}

	NTSTATUS
		NTAPI
		CloseSocket(
			__in PWSK_SOCKET WskSocket
		)
	{
		KEVENT          CompletionEvent = { 0 };
		PIRP            Irp = NULL;
		NTSTATUS        Status = STATUS_UNSUCCESSFUL;

		if (g_SocketsState != INITIALIZED || !WskSocket)
			return STATUS_INVALID_PARAMETER;

		Status = InitWskData(&Irp, &CompletionEvent);
		if (!NT_SUCCESS(Status)) {
			LOG_DEBUG("CloseSocket(): InitWskData() failed with status 0x%08X\n", Status);
			return Status;
		}

		Status = ((PWSK_PROVIDER_BASIC_DISPATCH)WskSocket->Dispatch)->WskCloseSocket(WskSocket, Irp);
		if (Status == STATUS_PENDING) {
			KeWaitForSingleObject(&CompletionEvent, Executive, KernelMode, FALSE, NULL);
			Status = Irp->IoStatus.Status;
		}

		IoFreeIrp(Irp);
		return Status;
	}


	NTSTATUS
		NTAPI
		Connect(
			__in PWSK_SOCKET        WskSocket,
			__in PSOCKADDR          RemoteAddress
		)
	{
		KEVENT          CompletionEvent = { 0 };
		PIRP            Irp = NULL;
		NTSTATUS        Status = STATUS_UNSUCCESSFUL;

		if (g_SocketsState != INITIALIZED || !WskSocket || !RemoteAddress)
			return STATUS_INVALID_PARAMETER;

		Status = InitWskData(&Irp, &CompletionEvent);
		if (!NT_SUCCESS(Status)) {
			LOG_DEBUG("Connect(): InitWskData() failed with status 0x%08X\n", Status);
			return Status;
		}

		Status = ((PWSK_PROVIDER_CONNECTION_DISPATCH)WskSocket->Dispatch)->WskConnect(
			WskSocket,
			RemoteAddress,
			0,
			Irp);
		if (Status == STATUS_PENDING) {
			KeWaitForSingleObject(&CompletionEvent, Executive, KernelMode, FALSE, NULL);
			Status = Irp->IoStatus.Status;
		}

		IoFreeIrp(Irp);
		return Status;
	}

	PWSK_SOCKET
		NTAPI
		SocketConnect(
			__in USHORT             SocketType,
			__in ULONG              Protocol,
			__in PSOCKADDR  RemoteAddress,
			__in PSOCKADDR  LocalAddress
		)
	{
		KEVENT                  CompletionEvent = { 0 };
		PIRP                    Irp = NULL;
		NTSTATUS                Status = STATUS_UNSUCCESSFUL;
		PWSK_SOCKET             WskSocket = NULL;

		if (g_SocketsState != INITIALIZED || !RemoteAddress || !LocalAddress)
			return NULL;

		Status = InitWskData(&Irp, &CompletionEvent);
		if (!NT_SUCCESS(Status)) {
			LOG_DEBUG("InitWskData() failed with status 0x%08X\n", Status);
			return NULL;
		}

		Status = g_WskProvider.Dispatch->WskSocketConnect(
			g_WskProvider.Client,
			SocketType,
			Protocol,
			LocalAddress,
			RemoteAddress,
			0,
			NULL,
			NULL,
			NULL,
			NULL,
			NULL,
			Irp);
		if (Status == STATUS_PENDING) {
			KeWaitForSingleObject(&CompletionEvent, Executive, KernelMode, FALSE, NULL);
			Status = Irp->IoStatus.Status;
		}

		WskSocket = NT_SUCCESS(Status) ? (PWSK_SOCKET)Irp->IoStatus.Information : NULL;

		IoFreeIrp(Irp);
		return WskSocket;
	}


	LONG
		NTAPI
		Send(
			__in PWSK_SOCKET        WskSocket,
			__in PVOID                      Buffer,
			__in ULONG                      BufferSize,
			__in ULONG                      Flags
		)
	{
		KEVENT          CompletionEvent = { 0 };
		PIRP            Irp = NULL;
		WSK_BUF         WskBuffer = { 0 };
		LONG            BytesSent = SOCKET_ERROR;
		NTSTATUS        Status = STATUS_UNSUCCESSFUL;

		if (g_SocketsState != INITIALIZED || !WskSocket || !Buffer || !BufferSize)
			return SOCKET_ERROR;

		Status = InitWskBuffer(Buffer, BufferSize, &WskBuffer);
		if (!NT_SUCCESS(Status)) {
			LOG_DEBUG("Send(): InitWskData() failed with status 0x%08X\n", Status);
			return SOCKET_ERROR;
		}

		Status = InitWskData(&Irp, &CompletionEvent);
		if (!NT_SUCCESS(Status)) {
			LOG_DEBUG("Send(): InitWskData() failed with status 0x%08X\n", Status);
			FreeWskBuffer(&WskBuffer);
			return SOCKET_ERROR;
		}

		Status = ((PWSK_PROVIDER_CONNECTION_DISPATCH)WskSocket->Dispatch)->WskSend(
			WskSocket,
			&WskBuffer,
			Flags,
			Irp);
		if (Status == STATUS_PENDING) {
			KeWaitForSingleObject(&CompletionEvent, Executive, KernelMode, FALSE, NULL);
			Status = Irp->IoStatus.Status;
		}

		BytesSent = NT_SUCCESS(Status) ? (LONG)Irp->IoStatus.Information : SOCKET_ERROR;

		IoFreeIrp(Irp);
		FreeWskBuffer(&WskBuffer);
		return BytesSent;
	}

	LONG
		NTAPI
		SendTo(
			__in PWSK_SOCKET        WskSocket,
			__in PVOID              Buffer,
			__in ULONG              BufferSize,
			__in_opt PSOCKADDR      RemoteAddress
		)
	{
		KEVENT          CompletionEvent = { 0 };
		PIRP            Irp = NULL;
		WSK_BUF         WskBuffer = { 0 };
		LONG            BytesSent = SOCKET_ERROR;
		NTSTATUS        Status = STATUS_UNSUCCESSFUL;

		if (g_SocketsState != INITIALIZED || !WskSocket || !Buffer || !BufferSize)
			return SOCKET_ERROR;

		Status = InitWskBuffer(Buffer, BufferSize, &WskBuffer);
		if (!NT_SUCCESS(Status)) {
			LOG_DEBUG("SendTo(): InitWskData() failed with status 0x%08X\n", Status);
			return SOCKET_ERROR;
		}

		Status = InitWskData(&Irp, &CompletionEvent);
		if (!NT_SUCCESS(Status)) {
			LOG_DEBUG("SendTo(): InitWskData() failed with status 0x%08X\n", Status);
			FreeWskBuffer(&WskBuffer);
			return SOCKET_ERROR;
		}

		Status = ((PWSK_PROVIDER_DATAGRAM_DISPATCH)WskSocket->Dispatch)->WskSendTo(
			WskSocket,
			&WskBuffer,
			0,
			RemoteAddress,
			0,
			NULL,
			Irp);
		if (Status == STATUS_PENDING) {
			KeWaitForSingleObject(&CompletionEvent, Executive, KernelMode, FALSE, NULL);
			Status = Irp->IoStatus.Status;
		}

		BytesSent = NT_SUCCESS(Status) ? (LONG)Irp->IoStatus.Information : SOCKET_ERROR;

		IoFreeIrp(Irp);
		FreeWskBuffer(&WskBuffer);
		return BytesSent;
	}

	LONG
		NTAPI
		Receive(
			__in  PWSK_SOCKET       WskSocket,
			__out PVOID                     Buffer,
			__in  ULONG                     BufferSize,
			__in  ULONG                     Flags
		)
	{
		KEVENT          CompletionEvent = { 0 };
		PIRP            Irp = NULL;
		WSK_BUF         WskBuffer = { 0 };
		LONG            BytesReceived = SOCKET_ERROR;
		NTSTATUS        Status = STATUS_UNSUCCESSFUL;

		if (g_SocketsState != INITIALIZED || !WskSocket || !Buffer || !BufferSize)
			return SOCKET_ERROR;

		Status = InitWskBuffer(Buffer, BufferSize, &WskBuffer);
		if (!NT_SUCCESS(Status)) {
			LOG_DEBUG("Receive(): InitWskData() failed with status 0x%08X\n", Status);
			return SOCKET_ERROR;
		}

		Status = InitWskData(&Irp, &CompletionEvent);
		if (!NT_SUCCESS(Status)) {
			LOG_DEBUG("Receive(): InitWskData() failed with status 0x%08X\n", Status);
			FreeWskBuffer(&WskBuffer);
			return SOCKET_ERROR;
		}

		Status = ((PWSK_PROVIDER_CONNECTION_DISPATCH)WskSocket->Dispatch)->WskReceive(
			WskSocket,
			&WskBuffer,
			Flags,
			Irp);
		if (Status == STATUS_PENDING) {
			KeWaitForSingleObject(&CompletionEvent, Executive, KernelMode, FALSE, NULL);
			Status = Irp->IoStatus.Status;
		}

		BytesReceived = NT_SUCCESS(Status) ? (LONG)Irp->IoStatus.Information : SOCKET_ERROR;

		IoFreeIrp(Irp);
		FreeWskBuffer(&WskBuffer);
		return BytesReceived;
	}

	LONG
		NTAPI
		ReceiveFrom(
			__in  PWSK_SOCKET       WskSocket,
			__out PVOID                     Buffer,
			__in  ULONG                     BufferSize,
			__out_opt PSOCKADDR     RemoteAddress,
			__out_opt PULONG        ControlFlags
		)
	{
		KEVENT          CompletionEvent = { 0 };
		PIRP            Irp = NULL;
		WSK_BUF         WskBuffer = { 0 };
		LONG            BytesReceived = SOCKET_ERROR;
		NTSTATUS        Status = STATUS_UNSUCCESSFUL;

		if (g_SocketsState != INITIALIZED || !WskSocket || !Buffer || !BufferSize)
			return SOCKET_ERROR;

		Status = InitWskBuffer(Buffer, BufferSize, &WskBuffer);
		if (!NT_SUCCESS(Status)) {
			LOG_DEBUG("ReceiveFrom(): InitWskData() failed with status 0x%08X\n", Status);
			return SOCKET_ERROR;
		}

		Status = InitWskData(&Irp, &CompletionEvent);
		if (!NT_SUCCESS(Status)) {
			LOG_DEBUG("ReceiveFrom(): InitWskData() failed with status 0x%08X\n", Status);
			FreeWskBuffer(&WskBuffer);
			return SOCKET_ERROR;
		}

		Status = ((PWSK_PROVIDER_DATAGRAM_DISPATCH)WskSocket->Dispatch)->WskReceiveFrom(
			WskSocket,
			&WskBuffer,
			0,
			RemoteAddress,
			0,
			NULL,
			ControlFlags,
			Irp);
		if (Status == STATUS_PENDING) {
			KeWaitForSingleObject(&CompletionEvent, Executive, KernelMode, FALSE, NULL);
			Status = Irp->IoStatus.Status;
		}

		BytesReceived = NT_SUCCESS(Status) ? (LONG)Irp->IoStatus.Information : SOCKET_ERROR;

		IoFreeIrp(Irp);
		FreeWskBuffer(&WskBuffer);
		return BytesReceived;
	}

	NTSTATUS
		NTAPI
		Bind(
			__in PWSK_SOCKET        WskSocket,
			__in PSOCKADDR          LocalAddress
		)
	{
		KEVENT          CompletionEvent = { 0 };
		PIRP            Irp = NULL;
		NTSTATUS        Status = STATUS_UNSUCCESSFUL;

		if (g_SocketsState != INITIALIZED || !WskSocket || !LocalAddress)
			return STATUS_INVALID_PARAMETER;

		Status = InitWskData(&Irp, &CompletionEvent);
		if (!NT_SUCCESS(Status)) {
			LOG_DEBUG("Bind(): InitWskData() failed with status 0x%08X\n", Status);
			return Status;
		}

		Status = ((PWSK_PROVIDER_CONNECTION_DISPATCH)WskSocket->Dispatch)->WskBind(
			WskSocket,
			LocalAddress,
			0,
			Irp);
		if (Status == STATUS_PENDING) {
			KeWaitForSingleObject(&CompletionEvent, Executive, KernelMode, FALSE, NULL);
			Status = Irp->IoStatus.Status;
		}

		IoFreeIrp(Irp);
		return Status;
	}


	PWSK_SOCKET
		NTAPI
		Accept(
			__in PWSK_SOCKET        WskSocket,
			__out_opt PSOCKADDR     LocalAddress,
			__out_opt PSOCKADDR     RemoteAddress
		)
	{
		KEVENT                  CompletionEvent = { 0 };
		PIRP                    Irp = NULL;
		NTSTATUS                Status = STATUS_UNSUCCESSFUL;
		PWSK_SOCKET             AcceptedSocket = NULL;

		if (g_SocketsState != INITIALIZED || !WskSocket)
			return NULL;

		Status = InitWskData(&Irp, &CompletionEvent);
		if (!NT_SUCCESS(Status)) {
			LOG_DEBUG("Accept(): InitWskData() failed with status 0x%08X\n", Status);
			return NULL;
		}

		Status = ((PWSK_PROVIDER_LISTEN_DISPATCH)WskSocket->Dispatch)->WskAccept(
			WskSocket,
			0,
			NULL,
			NULL,
			LocalAddress,
			RemoteAddress,
			Irp);
		if (Status == STATUS_PENDING) {
			KeWaitForSingleObject(&CompletionEvent, Executive, KernelMode, FALSE, NULL);
			Status = Irp->IoStatus.Status;
		}

		AcceptedSocket = NT_SUCCESS(Status) ? (PWSK_SOCKET)Irp->IoStatus.Information : NULL;

		IoFreeIrp(Irp);
		return AcceptedSocket;
	}

	NTSTATUS NTAPI ResolveName(
		_In_ PUNICODE_STRING NodeName,
		_In_ PUNICODE_STRING ServiceName,
		_In_opt_ PADDRINFOEXW Hints,
		__out PSOCKADDR_IN ResolvedAddress
	)
	{
		KEVENT                  CompletionEvent = { 0 };
		PIRP                    Irp = NULL;
		auto Status = InitWskData(&Irp, &CompletionEvent);
		if (!NT_SUCCESS(Status)) {
			LOG_DEBUG("ResolveName(): InitWskData() failed with status 0x%08X\n", Status);
			return NULL;
		}
		PADDRINFOEXW Results = NULL, AddrInfo = NULL;
		Status = g_WskProvider.Dispatch->WskGetAddressInfo(g_WskProvider.Client,
			NodeName,
			ServiceName,
			NS_ALL,
			nullptr,
			Hints,
			&Results,
			nullptr,
			nullptr,
			Irp);

		KeWaitForSingleObject(&CompletionEvent, Executive,
			KernelMode, FALSE, NULL);

		Status = Irp->IoStatus.Status;

		IoFreeIrp(Irp);

		if (!NT_SUCCESS(Status)) {
			LOG_DEBUG("resolve status %x\r\n", Status);
			return Status;
		}

		AddrInfo = Results; // your code here
		if (AddrInfo != NULL) {
			*ResolvedAddress = *((PSOCKADDR_IN)(AddrInfo->ai_addr));
		}
		else {
			Status = STATUS_UNSUCCESSFUL;
			LOG_DEBUG("no addresses found\r\n");
		}

		g_WskProvider.Dispatch->WskFreeAddressInfo(
			g_WskProvider.Client,
			Results);

		return Status;
	}

	NTSTATUS
		NTAPI
		DisConnect(
			_In_ PWSK_SOCKET	WskSocket
		)
	{
		KEVENT          CompletionEvent = { 0 };
		PIRP            Irp = NULL;
		NTSTATUS        Status = STATUS_UNSUCCESSFUL;

		if (WskSocket == NULL) {
			return Status;
		}
		Status = InitWskData(&Irp, &CompletionEvent);
		if (!NT_SUCCESS(Status)) {
			LOG_DEBUG("Connect(): InitWskData() failed with status 0x%08X\n", Status);
			return Status;
		}
		Status = ((PWSK_PROVIDER_CONNECTION_DISPATCH)WskSocket->Dispatch)->WskDisconnect(
			WskSocket,
			NULL,
			0,
			Irp);
		if (Status == STATUS_PENDING) {
			KeWaitForSingleObject(&CompletionEvent, Executive, KernelMode, FALSE, NULL);
			Status = Irp->IoStatus.Status;
		}

		IoFreeIrp(Irp);
		return Status;

	}
};