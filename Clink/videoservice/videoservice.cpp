// videoservice.cpp : Defines the entry point for the console application.
//
#include "stdafx.h"

#include <signal.h>
#include <x_log/msglog.h>

#include "config.h"
#include "App.h"

#include <stdlib.h>

//#ifdef _DEBUG
//#define CRTDBG_MAP_ALLOC
//#include <crtdbg.h>
//#endif

#include <Windows.h>
#include <Dbghelp.h>


#include "Counter.h"
#include "optimizer.h"
#include "NvtHelper.h"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif


#ifdef _DEBUG
#include "vld\vld.h"
#endif

static bool isrun = true;

void sigHandle(int signo)
{
	isrun = false;
}


LONG WINAPI exception_filter(struct _EXCEPTION_POINTERS* ExceptionInfo)
{
	char buf[64];

	struct timeval tv;
	gettimeofday(&tv, NULL);

#if 0
	sprintf_s(buf, "%u", tv.tv_sec % 5);//::GetTickCount());

	std::string workpath = getWork() + "crash_dump_" + buf;
    HANDLE lhDumpFile = CreateFileA(workpath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL ,NULL);
#else
	sprintf_s(buf, "%u", SERVICE_PORT % 10);//tv.tv_sec % 5);//::GetTickCount());

	std::string workpath = getWork() + "crash_dump_" + buf;
    HANDLE lhDumpFile = CreateFileA(workpath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL ,NULL);
	if(INVALID_HANDLE_VALUE == lhDumpFile)
	{
		return EXCEPTION_EXECUTE_HANDLER;
	}
#endif
 
    MINIDUMP_EXCEPTION_INFORMATION loExceptionInfo;
    loExceptionInfo.ExceptionPointers = ExceptionInfo;
    loExceptionInfo.ThreadId = GetCurrentThreadId();
    loExceptionInfo.ClientPointers = TRUE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),lhDumpFile, MiniDumpWithFullMemory, &loExceptionInfo, NULL, NULL);
 
    CloseHandle(lhDumpFile);
 
    return EXCEPTION_EXECUTE_HANDLER;
}

int _tmain(int argc, _TCHAR* argv[])
{
	SetErrorMode(SEM_NOGPFAULTERRORBOX);

	WSADATA data;
	if(WSAStartup(MAKEWORD(2, 2), &data) != 0)
	{
		return -1;
	}

	log_init(NULL);

	//telnet�����
	NvtHelper nvt_sv(DBG_PORT);
	nvt_sv.run();

	if(Counter::instance()->init() != 0)
	{
		LOG(LEVEL_ERROR, "Init Counter failed");
		return -1;
	}

	if(initConfig(argc, argv) != 0)
	{
		LOG(LEVEL_ERROR, "Init Config failed");
		return -1;
	}

	LOG(LEVEL_INFO, "Init Config Success");

	// 
	SetUnhandledExceptionFilter(exception_filter);

	App app;
	if(app.Init(NULL) != 0)
	{
		LOG(LEVEL_ERROR, "Init Application failed");
		return -1;
	}

	LOG(LEVEL_INFO, "Init Application Success");

	if(app.Start() != 0)
	{
		LOG(LEVEL_ERROR, "Start Application failed");
		return -1;
	}

	LOG(LEVEL_INFO, "Start Application Success");
		
	signal(SIGBREAK, sigHandle);
	signal(SIGTERM, sigHandle);
	signal(SIGINT,  sigHandle);

	while(isrun)
	{
		Sleep(1000);
	}

	app.Stop();
	LOG(LEVEL_INFO, "Stop Application Success");

	app.Deinit();
	LOG(LEVEL_INFO, "Deinit Application Success");

	Counter::instance()->deinit();

	//telnet�����
	nvt_sv.quit();

	log_deinit();

#ifdef _DEBUG
	_CrtDumpMemoryLeaks();
#endif 

	WSACleanup();

	return 0;
}
