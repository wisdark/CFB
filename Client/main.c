#include <Windows.h>

#include "stdafx.h"

#include "../Common/common.h"
#include "client.h"
#include "device.h"
#include "utils.h"

#pragma comment(lib, "Advapi32.lib") // for privilege check and driver/service {un}loading

static HANDLE g_hService;
static HANDLE g_hSCManager;


/**
 *
 */
BOOL AssignPrivilegeToSelf(LPWSTR lpPrivilegeName)
{
	HANDLE hToken;
	BOOL bRes = FALSE;
	LUID Luid;

	bRes = OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &hToken);

	if (bRes)
	{
		bRes = LookupPrivilegeValue(NULL, lpPrivilegeName, &Luid);

		if (bRes)
		{
			LUID_AND_ATTRIBUTES Privilege = {
				.Luid = Luid,
				.Attributes = SE_PRIVILEGE_ENABLED
			};

			TOKEN_PRIVILEGES NewPrivs = {
				.PrivilegeCount = 1,
				.Privileges[0].Luid = Luid
			};

			bRes = AdjustTokenPrivileges(hToken,
				FALSE,
				&NewPrivs,
				sizeof(TOKEN_PRIVILEGES),
				(PTOKEN_PRIVILEGES)NULL,
				(PDWORD)NULL) != 0;
		}

		CloseHandle(hToken);
	}

	return bRes;
}

/**
 *
 */
BOOL HasPrivilege(LPWSTR lpPrivName, PBOOL lpHasPriv)
{
	LUID Luid;
	HANDLE hToken;
	BOOL bRes, bHasPriv;

	bRes = LookupPrivilegeValue(NULL, lpPrivName, &Luid);
	if (!bRes)
		goto fail;

	LUID_AND_ATTRIBUTES Privilege = {
		.Luid = Luid,
		.Attributes = SE_PRIVILEGE_ENABLED | SE_PRIVILEGE_ENABLED_BY_DEFAULT
	};
	PRIVILEGE_SET PrivSet = {
		.PrivilegeCount = 1,
		.Privilege[0] = Privilege
	};

	bRes = OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &hToken);
	if (!bRes)
		goto fail;

	bRes = PrivilegeCheck(hToken, &PrivSet, &bHasPriv);
	if (!bRes)
		goto fail;

	*lpHasPriv = bHasPriv;
	return TRUE;

fail:
	return FALSE;
}


/**
 *
 */
BOOL RunInitializationChecks()
{
	BOOL IsSeDebugEnabled;

	xlog(LOG_INFO, L"Checking for Debug privilege...\n");

	if (!HasPrivilege(L"SeDebugPrivilege", &IsSeDebugEnabled))
	{
		PrintError(L"HasPrivilege()");
		return FALSE;
	}

	if (!IsSeDebugEnabled)
	{
		xlog(LOG_WARNING, L"SeDebugPrivilege is not enabled, trying to enable...\n");
		if (AssignPrivilegeToSelf(L"SeDebugPrivilege") == FALSE)
		{
			xlog(LOG_CRITICAL, L"SeDebugPrivilege is required for %s to run\n", CFB_PROGRAM_NAME_SHORT);
			xlog(LOG_INFO, L"Hint: Are you running as Administrator?\n");
			PrintError(L"AssignPrivilegeToSelf");
			return FALSE;
		}
	}

	xlog(LOG_SUCCESS, L"Got SeDebugPrivilege, resuming...\n");

	return TRUE;
}



/**
*
*/
VOID StartInterpreter()
{
	if (!OpenCfbDevice())
	{
		xlog(LOG_CRITICAL, L"Failed to get a handle to '%s'\n", CFB_USER_DEVICE_NAME);
		xlog(LOG_INFO, L"Hint: is the driver registered?\n");
		return;
	}

	DWORD dwCliTid;

	HANDLE hCli = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)RunInterpreter, NULL, 0, &dwCliTid);
	if (!hCli)
	{
		xlog(LOG_CRITICAL, L"Fatal! Failed to start interpreter... Reason: %lu\n", GetLastError());
	}
	else
	{
		xlog(LOG_SUCCESS, L"Interpreter thread started as TID=%d\n", dwCliTid);
		WaitForSingleObject(hCli, INFINITE);
	}

	CloseCfbDevice();

	return;
}


/*++

--*/
#define CFB_DRIVER_PATH L".\\Driver.sys"
#define CFB_SERVICE_NAME L"IrpDumper"
#define CFB_SERVICE_DESCRIPTION L"Dump IRP data from hooked drivers."

BOOL LoadDriver()
{
	xlog(LOG_DEBUG, L"Loading '%s'\n", CFB_DRIVER_PATH);

	g_hSCManager = OpenSCManager(L"", SERVICES_ACTIVE_DATABASE, SC_MANAGER_CREATE_SERVICE);

	if (!g_hSCManager)
	{
		PrintError(L"OpenSCManager()");
		return FALSE;
	}

	xlog(LOG_DEBUG, L"Create or open the service '%s'\n", CFB_SERVICE_NAME);

	HANDLE hService = CreateService(g_hSCManager,
		CFB_SERVICE_NAME,
		CFB_SERVICE_DESCRIPTION,
		SERVICE_START | DELETE | SERVICE_STOP,
		SERVICE_KERNEL_DRIVER,
		SERVICE_DEMAND_START,
		SERVICE_ERROR_IGNORE,
		CFB_DRIVER_PATH,
		NULL, NULL, NULL, NULL, NULL);

	//
	// if the service was already registered, just open it
	//
	if (!hService && GetLastError() == ERROR_DUPLICATE_SERVICE_NAME)
	{
		hService = OpenService(g_hSCManager, CFB_SERVICE_NAME, SERVICE_START | DELETE | SERVICE_STOP);
	}
	else
	{
		PrintError(L"CreateService()");
		return FALSE;
	}

	//
	// start the service
	//

	xlog(LOG_DEBUG, L"Starting service '%s'\n", CFB_SERVICE_NAME);

	return StartService(hService, 0, NULL);
}


/*++

Stops and unloads the service to the driver.
--*/
BOOL UnloadDriver()
{
	SERVICE_STATUS ServiceStatus;

	xlog(LOG_DEBUG, L"Stopping service '%s'\n", CFB_SERVICE_NAME);

	if (!ControlService(g_hService, SERVICE_CONTROL_STOP, &ServiceStatus))
	{
		PrintError(L"ControlService");
		return FALSE;
	}

	CloseServiceHandle(g_hService); // Should never fail

	if (!DeleteService(g_hService))
	{
		PrintError(L"DeleteService");
		return FALSE;
	}

	CloseServiceHandle(g_hSCManager); // Should never fail

	return TRUE;
}


/*++

The entrypoint for the loader.

--*/
int main()
{
	xlog(LOG_INFO, L"Starting %s (v%.02f) - by <%s>\n", CFB_PROGRAM_NAME_SHORT, CFB_VERSION, CFB_AUTHOR);
#ifdef _DEBUG
	xlog(LOG_DEBUG, L"DEBUG mode on\n");
#endif

	//
	// Check the privileges
	//
	if ( !RunInitializationChecks() )
		return -1;


	//
	// Load the driver, and the service
	//
	if (!LoadDriver())
		return -1;


	//
	// Launch the prompt
	//
	StartInterpreter();


	//
	// Unload the service, and the driver
	//
	if (!UnloadDriver())
		return -1;


	xlog(LOG_INFO, L"Thanks for using %s, have a nice day! - %s\n", CFB_PROGRAM_NAME_SHORT, CFB_AUTHOR);
    return 0;
}

