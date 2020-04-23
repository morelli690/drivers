#include <ntddk.h>

#include "config.h"
#include "new.h"
#include "delete.h"
#include "remote_thread_creation.h"
#include "new_processes_cache.h"

void DriverUnload(PDRIVER_OBJECT);

NTSTATUS CreateCloseDispatch(PDEVICE_OBJECT, PIRP);
NTSTATUS ReadDispatch(PDEVICE_OBJECT, PIRP);

void ProcessNotifyRoutine(PEPROCESS, HANDLE, PPS_CREATE_NOTIFY_INFO);
void ThreadNotifyRoutine(HANDLE, HANDLE, BOOLEAN);

LIST_ENTRY g_remote_thread_creations_list_head;
NewProcessesCache* g_new_processes_cache;

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object, PUNICODE_STRING)
{
	driver_object->DriverUnload = DriverUnload;

	driver_object->MajorFunction[IRP_MJ_CREATE] = CreateCloseDispatch;
	driver_object->MajorFunction[IRP_MJ_CLOSE] = CreateCloseDispatch;
	driver_object->MajorFunction[IRP_MJ_READ] = ReadDispatch;

	UNICODE_STRING device_name;
	::RtlInitUnicodeString(&device_name, config::kDeviceName);

	PDEVICE_OBJECT device_object;

	auto status = ::IoCreateDevice(
		driver_object,
		0,
		&device_name,
		FILE_DEVICE_UNKNOWN,
		FILE_DEVICE_SECURE_OPEN,
		true,
		&device_object
	);

	if (!NT_SUCCESS(status))
	{
		::KdPrint(("[-] Failed to create a device object.\n"));
		return status;
	}

	UNICODE_STRING symbolic_link;
	::RtlInitUnicodeString(&symbolic_link, config::kSymbolicLink);

	status = ::IoCreateSymbolicLink(
		&symbolic_link,
		&device_name
	);

	if (!NT_SUCCESS(status))
	{
		::IoDeleteDevice(device_object);
		::KdPrint(("[-] Failed to create a symbolic link.\n"));
		return status;
	}

	status = ::PsSetCreateProcessNotifyRoutineEx(ProcessNotifyRoutine, FALSE);

	if (!NT_SUCCESS(status))
	{
		::IoDeleteDevice(device_object);
		::IoDeleteSymbolicLink(&symbolic_link);
		::KdPrint(("[-] Failed to create a process notify routine.\n", status));
		return status;
	}

	status = ::PsSetCreateThreadNotifyRoutine(ThreadNotifyRoutine);

	if (!NT_SUCCESS(status))
	{
		::IoDeleteDevice(device_object);
		::IoDeleteSymbolicLink(&symbolic_link);
		::PsSetCreateProcessNotifyRoutineEx(ProcessNotifyRoutine, TRUE);
		::KdPrint(("[-] Failed to create a thread notify routine.\n"));
		return status;
	}

	::InitializeListHead(&g_remote_thread_creations_list_head);

	g_new_processes_cache = new (PagedPool, config::kDriverTag) NewProcessesCache();

	if (!g_new_processes_cache)
	{
		::IoDeleteDevice(device_object);
		::IoDeleteSymbolicLink(&symbolic_link);
		::PsSetCreateProcessNotifyRoutineEx(ProcessNotifyRoutine, TRUE);
		::PsRemoveCreateThreadNotifyRoutine(ThreadNotifyRoutine);
		::KdPrint(("[-] Failed to create a new processes cache.\n"));
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	::KdPrint(("[+] Loaded RemoteThreadDetector successfully.\n"));

	return STATUS_SUCCESS;
}

void DriverUnload(PDRIVER_OBJECT driver_object)
{
	UNICODE_STRING symbolic_link;
	::RtlInitUnicodeString(&symbolic_link, config::kSymbolicLink);

	::IoDeleteDevice(driver_object->DeviceObject);
	::IoDeleteSymbolicLink(&symbolic_link);

	::PsSetCreateProcessNotifyRoutineEx(ProcessNotifyRoutine, TRUE);
	::PsRemoveCreateThreadNotifyRoutine(ThreadNotifyRoutine);

	while (!::IsListEmpty(&g_remote_thread_creations_list_head))
	{
		auto entry = ::RemoveTailList(&g_remote_thread_creations_list_head);
		auto remote_thread_creation_entry = CONTAINING_RECORD(entry, RemoteThreadCreationEntry, list_entry);

		delete remote_thread_creation_entry;
	}

	::KdPrint(("[+] Unloaded RemoteThreadDetector successfully.\n"));
}

NTSTATUS CreateCloseDispatch(PDEVICE_OBJECT, PIRP irp)
{
	irp->IoStatus.Status = STATUS_SUCCESS;
	irp->IoStatus.Information = 0;

	::IoCompleteRequest(irp, IO_NO_INCREMENT);

	return STATUS_SUCCESS;
}

NTSTATUS ReadDispatch(PDEVICE_OBJECT, PIRP irp)
{
	irp->IoStatus.Status = STATUS_SUCCESS;
	irp->IoStatus.Information = 0;

	::IoCompleteRequest(irp, IO_NO_INCREMENT);

	return STATUS_SUCCESS;
}

void ProcessNotifyRoutine(PEPROCESS, HANDLE process_id, PPS_CREATE_NOTIFY_INFO create_info)
{
	if (create_info)
	{
		::KdPrint(("[*] Process %d was created.\n", process_id));
		g_new_processes_cache->AddProcess(::HandleToULong(process_id));
	}
}

void ThreadNotifyRoutine(HANDLE process_id, HANDLE thread_id, BOOLEAN create)
{
	if (create)
	{
		HANDLE creator_process_id = ::PsGetCurrentProcessId();

		if (process_id != creator_process_id &&
			::HandleToULong(creator_process_id) != 4 &&
			!g_new_processes_cache->IsNewlyCreated(::HandleToULong(process_id)))
		{
			RemoteThreadCreationEntry* entry =
				new (PagedPool, config::kDriverTag) RemoteThreadCreationEntry();

			if (!entry)
			{
				::KdPrint(("[-] Failed to log a remote thread creation detected due to insufficient memory.\n"));
				return;
			}

			entry->remote_thread_creation.thread_id = ::HandleToULong(thread_id);
			entry->remote_thread_creation.process_id = ::HandleToULong(process_id);
			entry->remote_thread_creation.creator_process_id = ::HandleToULong(creator_process_id);

			// race condition, should use a mutex / executive resource
			::InsertTailList(&g_remote_thread_creations_list_head, &entry->list_entry);

			::KdPrint(("[*] Thread %d in process %d was created remotely from process %d.\n",
				thread_id, process_id, creator_process_id));
		}
	}
}