//////////////////////////////////////////////////////////////////////////////
//
// This source code is part of SuRun
//
// Some sources in this project evolved from Microsoft sample code, some from 
// other free sources. The Shield Icons are taken from Windows XP Service Pack 
// 2 (xpsp2res.dll) 
// 
// Feel free to use the SuRun sources for your liking.
// 
//                                (c) Kay Bruns (http://kay-bruns.de), 2007,08
//////////////////////////////////////////////////////////////////////////////

// This is the main file for the SuRun service. This file handles:
// -SuRun Installation
// -SuRun Uninstallation
// -the Windows Service
// -putting the user to the SuRunners group
// -Terminating a Process for "Restart as admin..."
// -requesting permission/password in the logon session of the user
// -putting the users password to the user process via WriteProcessMemory

#define _WIN32_WINNT 0x0500
#define WINVER       0x0500
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <tchar.h>
#include <lm.h>
#include <ntsecapi.h>
#include <USERENV.H>
#include <Psapi.h>
#include <Tlhelp32.h>
#include <Wtsapi32.h>
#include "Setup.h"
#include "Service.h"
#include "IsAdmin.h"
#include "CmdLine.h"
#include "WinStaDesk.h"
#include "ResStr.h"
#include "LogonDlg.h"
#include "LSA_laar.h"
#include "LSALogon.h"
#include "UserGroups.h"
#include "ReqAdmin.h"
#include "Helpers.h"
#include "TrayShowAdmin.h"
#include "WatchDog.h"
#include "DBGTrace.h"
#include "Resource.h"
#include "SuRunExt/SuRunExt.h"
#include "SuRunExt/SysMenuHook.h"

#pragma comment(lib,"Wtsapi32.lib")
#pragma comment(lib,"shlwapi.lib")
#pragma comment(lib,"Userenv.lib")
#pragma comment(lib,"AdvApi32.lib")
#pragma comment(lib,"PSAPI.lib")

#ifndef _DEBUG

  #ifdef _WIN64
    #pragma comment(lib,"SuRunExt/ReleaseUx64/SuRunExt.lib")
  #else  _WIN64
    #ifdef _SR32
      #pragma comment(lib,"SuRunExt/ReleaseUsr32/SuRunExt32.lib")
    #else _SR32
      #pragma comment(lib,"SuRunExt/ReleaseU/SuRunExt.lib")
    #endif _SR32
  #endif _WIN64

#else _DEBUG

  #ifdef _WIN64
    #pragma comment(lib,"SuRunExt/DebugUx64/SuRunExt.lib")
  #else  _WIN64
    #pragma comment(lib,"SuRunExt/DebugU/SuRunExt.lib")
  #endif _WIN64

#endif _DEBUG

//////////////////////////////////////////////////////////////////////////////
// 
//  Globals
// 
//////////////////////////////////////////////////////////////////////////////

static SERVICE_STATUS_HANDLE g_hSS=0;
static SERVICE_STATUS g_ss= {0};
static HANDLE g_hPipe=INVALID_HANDLE_VALUE;
CResStr SvcName(IDS_SERVICE_NAME);

RUNDATA g_RunData={0};
TCHAR g_RunPwd[PWLEN]={0};//here, for historical reasons; olny used for RunAs

int g_RetVal=0;
bool g_CliIsAdmin=FALSE;

//////////////////////////////////////////////////////////////////////////////
// 
// CheckCliProcess:
// 
// checks if rd.CliProcessId is this exe and if rd is g_RunData of the calling
// process equals rd, copies the clients g_RunData to our g_RunData
//////////////////////////////////////////////////////////////////////////////
DWORD CheckCliProcess(RUNDATA& rd)
{
  g_CliIsAdmin=FALSE;
  if (rd.CliProcessId==GetCurrentProcessId())
    return 0;
  HANDLE hProcess=OpenProcess(PROCESS_ALL_ACCESS,FALSE,rd.CliProcessId);
  if (!hProcess)
    return 0;
  //g_CliIsAdmin
  {
    HANDLE hTok=NULL;
    HANDLE hThread=OpenThread(THREAD_ALL_ACCESS,FALSE,rd.CliThreadId);
    if (hThread)
    {
      OpenThreadToken(hThread,TOKEN_DUPLICATE,FALSE,&hTok);
      CloseHandle(hThread);
    }
    if ((!hTok)&&(!OpenProcessToken(hProcess,TOKEN_DUPLICATE,&hTok)))
      return CloseHandle(hProcess),0;
    g_CliIsAdmin=IsAdmin(hTok)!=0;
    CloseHandle(hTok);
  }
  SIZE_T s;
  //Check if the calling process is this Executable:
  {
    DWORD d;
    HMODULE hMod;
    TCHAR f1[MAX_PATH];
    TCHAR f2[MAX_PATH];
    if (!GetModuleFileName(0,f1,MAX_PATH))
      return CloseHandle(hProcess),0;
    if(!EnumProcessModules(hProcess,&hMod,sizeof(hMod),&d))
      return CloseHandle(hProcess),0;
    if(GetModuleFileNameEx(hProcess,hMod,f2,MAX_PATH)==0)
      return CloseHandle(hProcess),0;
    if(_tcsicmp(f1,f2)!=0)
    {
      DBGTrace2("Invalid Process! %s != %s !",f1,f2);
      return CloseHandle(hProcess),0;
    }
  }
  //Since it's the same process, g_RunData has the same address!
  if (!ReadProcessMemory(hProcess,&g_RunData,&g_RunData,sizeof(RUNDATA),&s))
  {
    DBGTrace1("ReadProcessMemory failed: %s",GetLastErrorNameStatic());
    return CloseHandle(hProcess),0;
  }
  CloseHandle(hProcess);
  if (sizeof(RUNDATA)!=s)
    return 0;
  if (memcmp(&rd,&g_RunData,sizeof(RUNDATA))!=0)
    return 1;
  return 2;
}

//////////////////////////////////////////////////////////////////////////////
// 
//  ResumeClient
// 
//////////////////////////////////////////////////////////////////////////////
BOOL ResumeClient(int RetVal,bool bWriteRunData=false) 
{
  HANDLE hProcess=OpenProcess(PROCESS_ALL_ACCESS,FALSE,g_RunData.CliProcessId);
  if (!hProcess)
  {
    DBGTrace1("OpenProcess failed: %s",GetLastErrorNameStatic());
    return FALSE;
  }
  SIZE_T n;
  //Since it's the same process, g_RetVal has the same address!
  if (!WriteProcessMemory(hProcess,&g_RetVal,&RetVal,sizeof(int),&n))
  {
    DBGTrace1("WriteProcessMemory failed: %s",GetLastErrorNameStatic());
    return CloseHandle(hProcess),FALSE;
  }
  if (sizeof(int)!=n)
    DBGTrace2("WriteProcessMemory invalid size %d != %d ",sizeof(int),n);
  if(bWriteRunData)
  {
    WriteProcessMemory(hProcess,&g_RunData,&g_RunData,sizeof(RUNDATA),&n);
    if (sizeof(RUNDATA)!=n)
      DBGTrace2("WriteProcessMemory invalid size %d != %d ",sizeof(RUNDATA),n);
  }
  CloseHandle(hProcess);
  return TRUE;
}

DWORD GetCsrssPid()
{
  HANDLE hProcessSnap = INVALID_HANDLE_VALUE;
  PROCESSENTRY32 pe32={0};
  DWORD dwRet=0;
  hProcessSnap =CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if(hProcessSnap == INVALID_HANDLE_VALUE)
    return 0;
  pe32.dwSize = sizeof(PROCESSENTRY32);
  if(Process32First(hProcessSnap, &pe32))
  {
    do
    {
      if(_tcsicmp(L"csrss.exe",pe32.szExeFile)==0)
      {
        dwRet=pe32.th32ProcessID;
        break;
      }
    }while (Process32Next(hProcessSnap,&pe32));
  }
  CloseHandle(hProcessSnap);
  return dwRet;
}

//////////////////////////////////////////////////////////////////////////////
// 
//  The Service:
// 
//////////////////////////////////////////////////////////////////////////////

VOID WINAPI SvcCtrlHndlr(DWORD dwControl)
{
  //service control handler
  if(dwControl==SERVICE_CONTROL_STOP)
  {
    g_ss.dwCurrentState   = SERVICE_STOP_PENDING; 
    g_ss.dwCheckPoint     = 0; 
    g_ss.dwWaitHint       = 0; 
    g_ss.dwWin32ExitCode  = 0; 
    SetServiceStatus(g_hSS,&g_ss);
    //Close Named Pipe
    HANDLE hPipe=g_hPipe;
    g_hPipe=INVALID_HANDLE_VALUE;
    //Connect to Pipe
    HANDLE sw=CreateFile(ServicePipeName,GENERIC_WRITE,0,0,OPEN_EXISTING,0,0);
    if (sw==INVALID_HANDLE_VALUE)
    {
      DisconnectNamedPipe(hPipe);
      sw=CreateFile(ServicePipeName,GENERIC_WRITE,0,0,OPEN_EXISTING,0,0);
    }
    //As g_hPipe is now INVALID_HANDLE_VALUE, the Service will exit
    CloseHandle(hPipe);
    return;
  } 
  if (g_hSS!=(SERVICE_STATUS_HANDLE)0) 
    SetServiceStatus(g_hSS,&g_ss);
}

//////////////////////////////////////////////////////////////////////////////
// 
//  GetProcessUserToken
// 
//////////////////////////////////////////////////////////////////////////////
HANDLE GetProcessUserToken(DWORD ProcId)
{
  HANDLE hToken=NULL;
  EnablePrivilege(SE_DEBUG_NAME);
  HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS,TRUE,ProcId);
  if (!hProc)
    return hToken;
  // Open impersonation token for process
  OpenProcessToken(hProc,TOKEN_IMPERSONATE|TOKEN_QUERY|TOKEN_DUPLICATE
    |TOKEN_ASSIGN_PRIMARY,&hToken);
  CloseHandle(hProc);
  if(!hToken)
    return hToken;
  HANDLE hUser=0;
  DuplicateTokenEx(hToken,MAXIMUM_ALLOWED,NULL,SecurityIdentification,
    TokenPrimary,&hUser); 
  return CloseHandle(hToken),hUser;
}

//////////////////////////////////////////////////////////////////////////////
// 
//  ShowTrayWarning
// 
//////////////////////////////////////////////////////////////////////////////
void ShowTrayWarning(LPCTSTR Text,int IconId,int TimeOut) 
{
  if ((!g_CliIsInAdmins) && GetHideFromUser(g_RunData.UserName))
    return;
  TCHAR cmd[4096]={0};
  GetSystemWindowsDirectory(cmd,4096);
  PathAppend(cmd,L"SuRun.exe");
  HANDLE hUser=GetSessionUserToken(g_RunData.SessionID);
  PROCESS_INFORMATION pi={0};
  STARTUPINFO si={0};
  si.cb	= sizeof(si);
  //Do not inherit Desktop from calling process, use Tokens Desktop
  TCHAR WinstaDesk[MAX_PATH];
  _stprintf(WinstaDesk,_T("%s\\%s"),g_RunData.WinSta,g_RunData.Desk);
  si.lpDesktop = WinstaDesk;
  //CreateProcessAsUser will only work from an NT System Account since the
  //Privilege SE_ASSIGNPRIMARYTOKEN_NAME is not present elsewhere
  EnablePrivilege(SE_ASSIGNPRIMARYTOKEN_NAME);
  EnablePrivilege(SE_INCREASE_QUOTA_NAME);
  //Show ToolTip "<Program> is running elevated"...
  if (CreateProcessAsUser(hUser,NULL,cmd,NULL,NULL,FALSE,
    CREATE_SUSPENDED|CREATE_UNICODE_ENVIRONMENT|DETACHED_PROCESS,NULL,NULL,&si,&pi))
  {
    //Tell SuRun to Say something:
    RUNDATA rd=g_RunData;
    rd.CliProcessId=0;
    rd.CliThreadId=pi.dwThreadId;
    rd.RetPtr=0;
    rd.RetPID=0;
    rd.IconId=IconId;
    rd.TimeOut=TimeOut;
    _tcscpy(rd.cmdLine,Text);
    DWORD_PTR n=0;
    if (!WriteProcessMemory(pi.hProcess,&g_RunData,&rd,sizeof(RUNDATA),&n))
      TerminateProcess(pi.hProcess,0);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  CloseHandle(hUser);
}

void TestEmptyAdminPasswords()
{
  switch(GetAdminNoPassWarn)
  {
  case APW_ALL:
    break;
  case APW_NR_SR_ADMIN:
    if (g_CliIsInSuRunners
      &&(!GetRestrictApps(g_RunData.UserName))
      &&(!GetNoRunSetup(g_RunData.UserName)))
      break;
    goto ChkAdmin;
  case APW_SURUN_ADMIN:
    if (g_CliIsInSuRunners)
      break;
  case APW_ADMIN:
ChkAdmin:
    if (g_CliIsAdmin)
      break;
  case APW_NONE:
  default:
    return;
  }
  USERLIST u;
  u.SetGroupUsers(DOMAIN_ALIAS_RID_ADMINS,g_RunData.SessionID,false);
  TCHAR un[4096]={0};
  for (int i=0;i<u.GetCount();i++) if (PasswordOK(u.GetUserName(i),0,TRUE))
  {
    DBGTrace1("Warning: %s is an empty password admin",u.GetUserName(i));
    _tcscat(un,u.GetUserName(i));
    _tcscat(un,_T("\n"));
  }
  if(un[0])
    ShowTrayWarning(CBigResStr(IDS_EMPTYPASS,un),IDI_SHIELD2,0);
}

VOID WINAPI ServiceMain(DWORD argc,LPTSTR *argv)
{
#ifdef _DEBUG_ENU
  SetThreadLocale(MAKELCID(MAKELANGID(LANG_ENGLISH,SUBLANG_ENGLISH_US),SORT_DEFAULT));
#endif _DEBUG_ENU
  zero(g_RunPwd);
  //service main
  argc;//unused
  argv;//unused
  DBGTrace( "SuRun Service starting");
  g_ss.dwServiceType      = SERVICE_WIN32_OWN_PROCESS; 
  g_ss.dwControlsAccepted = SERVICE_ACCEPT_STOP; 
  g_ss.dwCurrentState     = SERVICE_START_PENDING; 
  g_hSS                   = RegisterServiceCtrlHandler(SvcName,SvcCtrlHndlr); 
  if (g_hSS==(SERVICE_STATUS_HANDLE)0) 
    return; 
  //Steal token from csrss.exe to get SeCreateTokenPrivilege in Vista
  HANDLE hRunCsrss=GetProcessUserToken(GetCsrssPid());
  //Create Pipe:
  g_hPipe=CreateNamedPipe(ServicePipeName,
    PIPE_ACCESS_INBOUND|WRITE_DAC|FILE_FLAG_FIRST_PIPE_INSTANCE,
    PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT,
    1,0,0,NMPWAIT_USE_DEFAULT_WAIT,NULL);
  if (g_hPipe!=INVALID_HANDLE_VALUE)
  {
    AllowAccess(g_hPipe);
    //Set Service Status to "running"
    g_ss.dwCurrentState     = SERVICE_RUNNING; 
    g_ss.dwCheckPoint       = 0;
    g_ss.dwWaitHint         = 0;
    SetServiceStatus(g_hSS,&g_ss);
    DBGTrace( "SuRun Service running");
    while (g_hPipe!=INVALID_HANDLE_VALUE)
    {
      //Wait for a connection
      ConnectNamedPipe(g_hPipe,0);
      //Exit if g_hPipe==INVALID_HANDLE_VALUE
      if (g_hPipe==INVALID_HANDLE_VALUE)
        break;
      DWORD nRead=0;
      RUNDATA rd={0};
      //Read Client Process ID and command line
      ReadFile(g_hPipe,&rd,sizeof(rd),&nRead,0); 
      //Disconnect client
      DisconnectNamedPipe(g_hPipe);
      if ((nRead==sizeof(RUNDATA)) && (CheckCliProcess(rd)==2))
      {
        if ((_tcsicmp(g_RunData.cmdLine,_T("/TSATHREAD"))==0)&&(g_RunData.KillPID==0xFFFFFFFF))
        {
          TestEmptyAdminPasswords();
          CloseHandle(CreateThread(0,0,TSAThreadProc,(VOID*)(DWORD_PTR)g_RunData.CliProcessId,0,0));
          continue;
        }
        if (!g_RunData.bRunAs)
        {
          DWORD wlf=GetWhiteListFlags(g_RunData.UserName,g_RunData.cmdLine,-1);
          bool bNotInList=wlf==-1;
          if(bNotInList)
            wlf=0;
          if  (bNotInList 
            && GetRestrictApps(g_RunData.UserName) 
            && (_tcsicmp(g_RunData.cmdLine,_T("/SETUP"))!=0))
          {
            ResumeClient(g_RunData.bShlExHook?RETVAL_SX_NOTINLIST:RETVAL_RESTRICT);
            //DBGTrace2("Restriction WhiteList MisMatch: %s: %s",g_RunData.UserName,g_RunData.cmdLine)
            continue;
          }
          //check if the requested App is Flagged with AutoCancel
          if (wlf&FLAG_AUTOCANCEL)
          {
            //Access denied!
            ResumeClient((g_RunData.bShlExHook)?RETVAL_SX_NOTINLIST:RETVAL_CANCELLED);
            //DBGTrace2("ShellExecute AutoCancel WhiteList MATCH: %s: %s",g_RunData.UserName,g_RunData.cmdLine)
            continue;
          }
          //check if the requested App is in the ShellExecHook-Runlist
          if (g_RunData.bShlExHook)
          {
            //check if the requested App is Flagged with AutoCancel
            if (wlf&FLAG_CANCEL_SX)
            {
              //Access denied!
              ResumeClient((g_RunData.bShlExHook)?RETVAL_SX_NOTINLIST:RETVAL_CANCELLED);
              //DBGTrace2("ShellExecute AutoCancel WhiteList MATCH: %s: %s",g_RunData.UserName,g_RunData.cmdLine);
              continue;
            }
            //Only SuRunners can use the hooks
            if (!g_CliIsInSuRunners)
            {
              ResumeClient(RETVAL_SX_NOTINLIST);
              continue;
            }
            if ((!(wlf&FLAG_SHELLEXEC))
              //check for requireAdministrator Manifest and
              //file names *setup*;*install*;*update*;*.msi;*.msc
              && (!RequiresAdmin(g_RunData.cmdLine)))
            {
              ResumeClient(RETVAL_SX_NOTINLIST);
              //DBGTrace2("ShellExecute WhiteList MisMatch: %s: %s",g_RunData.UserName,g_RunData.cmdLine)
              continue;
            }
            //DBGTrace2("ShellExecute WhiteList Match: %s: %s",g_RunData.UserName,g_RunData.cmdLine)
          }
        }//if (!g_RunData.bRunAs)
        //Process Check succeded, now start this exe in the calling processes
        //Terminal server session to get SwitchDesktop working:
        if (hRunCsrss)
        {
          DWORD SessionID=0;
          ProcessIdToSessionId(g_RunData.CliProcessId,&SessionID);
          if(SetTokenInformation(hRunCsrss,TokenSessionId,&SessionID,sizeof(DWORD)))
          {
            STARTUPINFO si={0};
            si.cb=sizeof(si);
            TCHAR WinstaDesk[MAX_PATH];
            _stprintf(WinstaDesk,_T("%s\\%s"),g_RunData.WinSta,g_RunData.Desk);
            si.lpDesktop = WinstaDesk;
            TCHAR cmd[4096]={0};
            GetSystemWindowsDirectory(cmd,4096);
            PathAppend(cmd,L"SuRun.exe");
            PathQuoteSpaces(cmd);
            _tcscat(cmd,L" /AskPID ");
            TCHAR PID[10];
            _tcscat(cmd,_itot(g_RunData.CliProcessId,PID,10));
            EnablePrivilege(SE_ASSIGNPRIMARYTOKEN_NAME);
            EnablePrivilege(SE_INCREASE_QUOTA_NAME);
            BYTE bRunCount=0;
TryAgain:
            PROCESS_INFORMATION pi={0};
            DWORD stTime=timeGetTime();
            if (CreateProcessAsUser(hRunCsrss,NULL,cmd,NULL,NULL,FALSE,
                  CREATE_UNICODE_ENVIRONMENT|HIGH_PRIORITY_CLASS,
                  0,NULL,&si,&pi))
            {
              bRunCount++;
              CloseHandle(pi.hThread);
              WaitForSingleObject(pi.hProcess,INFINITE);
              DWORD ex=0;
              GetExitCodeProcess(pi.hProcess,&ex);
              CloseHandle(pi.hProcess);
              if (ex!=(~pi.dwProcessId))
              {
                DBGTrace4("SuRun: Starting child process try %d failed after %d ms. "
                  L"Expected return value :%X real return value %x",
                  bRunCount,timeGetTime()-stTime,~pi.dwProcessId,ex);
                //For YZshadow: Give it four tries
                if ((bRunCount<4)&&(ex==STATUS_ACCESS_VIOLATION))
                  goto TryAgain;
                ResumeClient(RETVAL_ACCESSDENIED);
              }
            }else
              DBGTrace2("CreateProcessAsUser(%s) failed %s",cmd,GetLastErrorNameStatic());
          }else
            DBGTrace2("SetTokenInformation(TokenSessionId(%d)) failed %s",SessionID,GetLastErrorNameStatic());
        }
      }
      zero(rd);
    }
  }else
    DBGTrace1( "CreateNamedPipe failed %s",GetLastErrorNameStatic());
  CloseHandle(hRunCsrss);
  //Stop Service
  g_ss.dwCurrentState     = SERVICE_STOPPED; 
  g_ss.dwCheckPoint       = 0;
  g_ss.dwWaitHint         = 0;
  g_ss.dwWin32ExitCode    = 0; 
  DBGTrace( "SuRun Service stopped");
  SetServiceStatus(g_hSS,&g_ss);
}

//////////////////////////////////////////////////////////////////////////////
// 
//  KillProcess
// 
//////////////////////////////////////////////////////////////////////////////

// callback function for window enumeration
BOOL g_bKilledOne=FALSE;
static BOOL CALLBACK CloseAppEnum(HWND hwnd,LPARAM lParam )
{
  // no top level window, or invisible?
  if ((GetWindow(hwnd,GW_OWNER))||(!IsWindowVisible(hwnd)))
    return TRUE;
  TCHAR s[4096]={0};
  if ((!InternalGetWindowText(hwnd,s,countof(s)))||(s[0]==0))
    return TRUE;
  DWORD dwID;
  GetWindowThreadProcessId(hwnd, &dwID) ;
  if(dwID==(DWORD)lParam)
  {
    PostMessage(hwnd,WM_CLOSE,0,0) ;
    g_bKilledOne=TRUE;
  }
  return TRUE ;
}

void KillProcess(DWORD PID)
{
  if (!PID)
    return;
  HANDLE hProcess=OpenProcess(SYNCHRONIZE|PROCESS_TERMINATE,TRUE,PID);
  if(!hProcess)
    return;
  //Messages work on the same WinSta/Desk only
  SetProcWinStaDesk(g_RunData.WinSta,g_RunData.Desk);
  //Post WM_CLOSE to all Windows of PID
  g_bKilledOne=FALSE;
  EnumWindows(CloseAppEnum,(LPARAM)PID);
  //Give the Process time to close
  if ((!g_bKilledOne) || (WaitForSingleObject(hProcess,5000)!=WAIT_OBJECT_0))
    TerminateProcess(hProcess,0);
  CloseHandle(hProcess);
}

//////////////////////////////////////////////////////////////////////////////
// 
//  BeautifyCmdLine: replace shell namespace object names
// 
//////////////////////////////////////////////////////////////////////////////
LPCTSTR GetShellName(LPCTSTR clsid,LPCTSTR DefName)
{
  static TCHAR s[MAX_PATH];
  zero(s);
  GetRegStr(HKCR,CResStr(L"CLSID\\%s",clsid),L"LocalizedString",s,MAX_PATH);
  if(s[0]=='@')
  {
    LPTSTR c=_tcsrchr(s,',');
    if(c)
    {
      *c=0;
      c++;
      int i=_ttoi(c);
      HINSTANCE h=LoadLibrary(&s[1]);
      LoadString(h,(DWORD)-i,s,MAX_PATH);
      FreeLibrary(h);
    }
  }
  if (!s[0])
    GetRegStr(HKCR,CResStr(L"CLSID\\%s",clsid),L"",s,MAX_PATH);
  if (!s[0])
    _tcscpy(s,DefName);
  return s;
}

LPCTSTR BeautifyCmdLine(LPTSTR cmd)
{
  typedef struct {
    int nId;
    LPCTSTR clsid;
  }ShName;
  ShName shn[]=
  {
    {IDS_SHNAME1,_T("::{20D04FE0-3AEA-1069-A2D8-08002B30309D}")},
    {IDS_SHNAME2,_T("::{208D2C60-3AEA-1069-A2D7-08002B30309D}")},
    {IDS_SHNAME3,_T("::{7007ACC7-3202-11D1-AAD2-00805FC1270E}")},
    {IDS_SHNAME4,_T("::{21EC2020-3AEA-1069-A2DD-08002B30309D}")},
    {IDS_SHNAME5,_T("::{645FF040-5081-101B-9F08-00AA002F954E}")},
    {IDS_SHNAME6,_T("::{D20EA4E1-3957-11d2-A40B-0C5020524152}")},
    {IDS_SHNAME7,_T("::{D20EA4E1-3957-11d2-A40B-0C5020524153}")},
    {IDS_SHNAME8,_T("::{450D8FBA-AD25-11D0-98A8-0800361B1103}")}
  };
  static TCHAR c1[4096];
  zero(c1);
  _tcscpy(c1,cmd);
  bool bOk=false;
  for (int i=0;i<countof(shn);i++)
  {
    LPTSTR c=_tcsstr(c1,shn[i].clsid);
    if(c)
    {
      bOk=TRUE;
      *c=0;
      TCHAR c2[4096];
      _stprintf(c2,L"%s%s%s",
        c1,
        GetShellName(&shn[i].clsid[2],CResStr(shn[i].nId)),
        &c[_tcslen(shn[i].clsid)]);
      _tcscpy(c1,c2);
    }
  }
  if (bOk)
    _tcscpy(c1,CBigResStr(IDS_BEAUTIFIED,cmd,c1));
  return c1;
}

//int tx()
//{
//  TCHAR cmd[4096]={0};
//  _tcscpy(cmd,L"C:\\Windows\\Exploer.exe /e, ::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\::{208D2C60-3AEA-1069-A2D7-08002B30309D}\\::{21EC2020-3AEA-1069-A2DD-08002B30309D}\\::{871C5380-42A0-1069-A2EA-08002B30309D}");
//  DBGTrace1("%s",BeautifyCmdLine(cmd));
//
//  ::ExitProcess(0);
//  return 0;
//}
//int xtx=tx();

//////////////////////////////////////////////////////////////////////////////
// 
//  PrepareSuRun: Show Password/Permission Dialog on secure Desktop,
// 
//////////////////////////////////////////////////////////////////////////////
DWORD PrepareSuRun()
{
  zero(g_RunPwd);
  BOOL PwOk=FALSE;
  RegDelVal(HKLM,PASSWKEY,g_RunData.UserName);//Delete Password, keep time
  //Ask For Password?
  PwOk=GetSavePW &&(!PasswordExpired(g_RunData.UserName));
  if((!g_CliIsInSuRunners) && GetNoConvUser)
    return RETVAL_ACCESSDENIED;
  if (!PwOk)
    DeletePassword(g_RunData.UserName);
  else
  if (IsInWhiteList(g_RunData.UserName,g_RunData.cmdLine,FLAG_DONTASK))
    return UpdLastRunTime(g_RunData.UserName),RETVAL_OK;
  g_RunData.Groups=IsInSuRunnersOrAdmins(g_RunData.UserName,g_RunData.SessionID);
  if (HideSuRun(g_RunData.UserName,g_RunData.Groups))
    return RETVAL_CANCELLED;
  //Create the new desktop
  if (!CreateSafeDesktop(g_RunData.WinSta,g_RunData.Desk,GetBlurDesk,GetFadeDesk))
    return RETVAL_NODESKTOP;
  __try
  {
    //secure desktop created...
    if ((!g_CliIsInSuRunners)
      && (!BecomeSuRunner(g_RunData.UserName,g_RunData.SessionID,g_CliIsInAdmins,TRUE,0)))
      return RETVAL_CANCELLED;
    if (!g_CliIsInSuRunners)
    {
      PwOk=GetSavePW &&(!PasswordExpired(g_RunData.UserName));
      g_RunData.Groups=IS_IN_SURUNNERS;
    }
    //Is User Restricted?
    DWORD f=GetWhiteListFlags(g_RunData.UserName,g_RunData.cmdLine,-1);
    bool bNotInList=f==-1;
    if(bNotInList)
      f=0;
    if  (GetRestrictApps(g_RunData.UserName) && bNotInList)
      return g_RunData.bShlExHook?RETVAL_SX_NOTINLIST:RETVAL_RESTRICT;
    DWORD l=0;
    if (!PwOk)
    {
      l=LogonCurrentUser(g_RunData.UserName,g_RunPwd,f,g_RunData.bShlExHook?IDS_ASKAUTO:IDS_ASKOK,
          BeautifyCmdLine(g_RunData.cmdLine));
    }else
    {
      l=AskCurrentUserOk(g_RunData.UserName,f,g_RunData.bShlExHook?IDS_ASKAUTO:IDS_ASKOK,
        BeautifyCmdLine(g_RunData.cmdLine));
    }
    DeleteSafeDesktop(GetFadeDesk && ((l&1)==0));
    if((l&1)==0)
    {
      if (!GetNoRunSetup(g_RunData.UserName))
      {
        //Cancel:
        if(g_RunData.bShlExHook)
        {
          //ShellExecHook:
          SetWhiteListFlag(g_RunData.UserName,g_RunData.cmdLine,FLAG_CANCEL_SX,(l&2)!=0);
          if((l&2)!=0)
            SetWhiteListFlag(g_RunData.UserName,g_RunData.cmdLine,FLAG_SHELLEXEC,0);
        }else
        {
          //SuRun cmdline:
          SetWhiteListFlag(g_RunData.UserName,g_RunData.cmdLine,FLAG_AUTOCANCEL,(l&2)!=0);
          if((l&2)!=0)
            SetWhiteListFlag(g_RunData.UserName,g_RunData.cmdLine,FLAG_DONTASK,0);
        }
      }
      return RETVAL_CANCELLED;
    }
    //Ok:
    if (!GetNoRunSetup(g_RunData.UserName))
    {
      SetWhiteListFlag(g_RunData.UserName,g_RunData.cmdLine,FLAG_DONTASK,(l&2)!=0);
      if((l&2)!=0)
        SetWhiteListFlag(g_RunData.UserName,g_RunData.cmdLine,
        g_RunData.bShlExHook?FLAG_CANCEL_SX:FLAG_AUTOCANCEL,0);
      SetWhiteListFlag(g_RunData.UserName,g_RunData.cmdLine,FLAG_SHELLEXEC,(l&4)!=0);
    }
    UpdLastRunTime(g_RunData.UserName);
    return RETVAL_OK;
  }__except(1)
  {
    DBGTrace("FATAL: Exception in PrepareSuRun()");
    DeleteSafeDesktop(false);
    return RETVAL_CANCELLED;
  }
}

//////////////////////////////////////////////////////////////////////////////
// 
//  CheckServiceStatus
// 
//////////////////////////////////////////////////////////////////////////////

DWORD CheckServiceStatus(LPCTSTR ServiceName=SvcName)
{
  SC_HANDLE hdlSCM=OpenSCManager(0,0,SC_MANAGER_CONNECT|SC_MANAGER_ENUMERATE_SERVICE);
  if (hdlSCM==0) 
    return FALSE;
  SC_HANDLE hdlServ = OpenService(hdlSCM,ServiceName,SERVICE_QUERY_STATUS);
  if (!hdlServ) 
    return CloseServiceHandle(hdlSCM),FALSE;
  SERVICE_STATUS ss={0};
  if (!QueryServiceStatus(hdlServ,&ss))
  {
    CloseServiceHandle(hdlServ);
    return CloseServiceHandle(hdlSCM),FALSE;
  }
  CloseServiceHandle(hdlServ);
  CloseServiceHandle(hdlSCM);
  return ss.dwCurrentState;
}

//////////////////////////////////////////////////////////////////////////////
// 
//  Setup
// 
//////////////////////////////////////////////////////////////////////////////

BOOL Setup()
{
  //check if user name may not run setup:
  if (GetNoRunSetup(g_RunData.UserName))
  {
    if(!LogonAdmin(g_RunData.SessionID,IDS_NOADMIN2,g_RunData.UserName))
      return FALSE;
    return RunSetup(g_RunData.SessionID,g_RunData.UserName);
  }
  //check if user name needs to enter the password:
  if (GetReqPw4Setup(g_RunData.UserName))
  {
    if(!ValidateCurrentUser(g_RunData.UserName,IDS_PW4SETUP))
      return FALSE;
    return RunSetup(g_RunData.SessionID,g_RunData.UserName);
  }
  //only Admins and SuRunners may setup SuRun
  if (g_CliIsAdmin)
    return RunSetup(g_RunData.SessionID,g_RunData.UserName);
  //If no users should become SuRunners, ask for Admin credentials
  if ((!g_CliIsInSuRunners) && GetNoConvUser)
  {
    if(!LogonAdmin(g_RunData.SessionID,IDS_NOADMIN2,g_RunData.UserName))
      return FALSE;
    return RunSetup(g_RunData.SessionID,g_RunData.UserName);
  }
  if (g_CliIsInSuRunners 
    || BecomeSuRunner(g_RunData.UserName,g_RunData.SessionID,g_CliIsInAdmins,TRUE,0))
    return RunSetup(g_RunData.SessionID,g_RunData.UserName);
  return false;
}

//////////////////////////////////////////////////////////////////////////////
// 
//  LSAStartAdminProcess
// 
//////////////////////////////////////////////////////////////////////////////
HANDLE GetUserToken(DWORD SessionID,LPCTSTR UserName,LPTSTR Password,bool bNoAdmin)
{
  //Admin Token for SessionId
  HANDLE hUser=0;
  TCHAR un[2*UNLEN+2]={0};
  TCHAR dn[2*UNLEN+2]={0};
  _tcscpy(un,UserName);
  PathStripPath(un);
  _tcscpy(dn,UserName);
  PathRemoveFileSpec(dn);
  //Enable use of empty passwords for network logon
  BOOL bEmptyPWAllowed=FALSE;
  if(bNoAdmin)
    hUser=LSALogon(SessionID,un,dn,Password,bNoAdmin);
  else
    hUser=GetAdminToken(SessionID);
  return hUser;
}

#define GetSeparateProcess(k) GetRegInt(k,\
                    _T("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced"),\
                    _T("SeparateProcess"),0)
#define SetSeparateProcess(k,b) SetRegInt(k,\
                    _T("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced"),\
                    _T("SeparateProcess"),b)

#define DESKTOPPROXYCLASS   TEXT("Proxy Desktop")

BOOL CALLBACK KillProxyDesktopEnum(HWND hwnd, LPARAM lParam)
{
  TCHAR cn[MAX_PATH];
  GetClassName(hwnd, cn, countof(cn));
  if (_tcsicmp(cn, DESKTOPPROXYCLASS)==0)
  {
    if(lParam)
    {
      DWORD pid=0;
      GetWindowThreadProcessId(hwnd,&pid);
      if (pid==*((DWORD*)lParam))
      {
        SendMessage(hwnd,WM_CLOSE,0,0);
        *((DWORD*)lParam)=0;
        return FALSE;
      }
    }else
      SendMessage(hwnd,WM_CLOSE,0,0);
  }
  return TRUE;
}


DWORD LSAStartAdminProcess() 
{
  DWORD RetVal=RETVAL_ACCESSDENIED;
  //Get Admin User Token and Job object token
  HANDLE hAdmin=GetUserToken(g_RunData.SessionID,g_RunData.UserName,g_RunPwd,g_RunData.bRunAs);
  //Clear Password
  zero(g_RunPwd);
  if (!hAdmin)
  {
    DBGTrace("FATAL: Could not create user token!");
    return RetVal;
  }
  SetTokenInformation(hAdmin,TokenSessionId,&g_RunData.SessionID,sizeof(DWORD));
  PROCESS_INFORMATION pi={0};
  PROFILEINFO ProfInf = {sizeof(ProfInf),0,g_RunData.UserName};
  if(LoadUserProfile(hAdmin,&ProfInf))
  {
    void* Env=0;
    if (CreateEnvironmentBlock(&Env,hAdmin,FALSE))
    {
      STARTUPINFO si={0};
      si.cb	= sizeof(si);
      //Do not inherit Desktop from calling process, use Tokens Desktop
      TCHAR WinstaDesk[MAX_PATH];
      _stprintf(WinstaDesk,_T("%s\\%s"),g_RunData.WinSta,g_RunData.Desk);
      si.lpDesktop = WinstaDesk;
      //Special handling for Explorer:
      BOOL orgSP=1;
      BOOL bIsExplorer=FALSE;
      {
        TCHAR app[MAX_PATH]={0};
        GetSystemWindowsDirectory(app,4096);
        PathAppend(app,_T("explorer.exe"));
        TCHAR cmd[MAX_PATH]={0};
        _tcscpy(cmd,g_RunData.cmdLine);
        PathRemoveArgs(cmd);
        PathUnquoteSpaces(cmd);
        bIsExplorer=_tcscmp(cmd,app)==0;
      }
      if(bIsExplorer)
      {
        //Before Vista: kill Desktop Proxy
        if (LOBYTE(LOWORD(GetVersion()))<6)
        {
          //To start control Panel and other Explorer children we need to tell 
          //Explorer to open folders in a new proecess
          orgSP=GetSeparateProcess((HKEY)ProfInf.hProfile);
          if(!orgSP)
            SetSeparateProcess((HKEY)ProfInf.hProfile,1);
          //Messages work on the same WinSta/Desk only
          SetProcWinStaDesk(g_RunData.WinSta,g_RunData.Desk);
          //call DestroyWindow() for each "Desktop Proxy" Windows Class in an 
          //Explorer.exe, this will cause a new Explorer.exe to stay running
          EnumWindows(KillProxyDesktopEnum,0);
        }
        else //Vista and newer, use "/separate" command line option
        {
          TCHAR cmd[MAX_PATH]={0};
          _tcscpy(cmd,g_RunData.cmdLine);
          PathRemoveArgs(cmd);
          _tcscat(cmd,_T(" /SEPARATE, "));
          _tcscat(cmd,PathGetArgs(g_RunData.cmdLine));
          _tcscpy(g_RunData.cmdLine,cmd);
        }
      }
      //CreateProcessAsUser will only work from an NT System Account since the
      //Privilege SE_ASSIGNPRIMARYTOKEN_NAME is not present elsewhere
      EnablePrivilege(SE_ASSIGNPRIMARYTOKEN_NAME);
      EnablePrivilege(SE_INCREASE_QUOTA_NAME);
      if (CreateProcessAsUser(hAdmin,NULL,g_RunData.cmdLine,NULL,NULL,FALSE,
        CREATE_UNICODE_ENVIRONMENT,Env,g_RunData.CurDir,&si,&pi))
      {
        DBGTrace1("CreateProcessAsUser(%s) OK",g_RunData.cmdLine);
        if(bIsExplorer)
        {
          //Before Vista: wait for and kill Desktop Proxy
          if (LOBYTE(LOWORD(GetVersion()))<6)
          {
            //Messages work on the same WinSta/Desk only
            SetProcWinStaDesk(g_RunData.WinSta,g_RunData.Desk);
            //call DestroyWindow() for each "Desktop Proxy" Windows Class in an 
            //Explorer.exe, this will cause a new Explorer.exe to stay running
            CTimeOut to(10000);
            DWORD pid=pi.dwProcessId;
            while ((!to.TimedOut()) 
              && pid && EnumWindows(KillProxyDesktopEnum,(LPARAM)&pid)
              && (WaitForSingleObject(pi.hProcess,100)==WAIT_TIMEOUT))
              ;
          }
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        RetVal=RETVAL_OK;
        //ShellExec-Hook: We must return the PID and TID to fake CreateProcess:
        if((g_RunData.RetPID)&&(g_RunData.RetPtr))
        {
          pi.hThread=0;
          pi.hProcess=0;
          HANDLE hProcess=OpenProcess(PROCESS_VM_OPERATION|PROCESS_VM_WRITE,FALSE,g_RunData.RetPID);
          if (hProcess)
          {
            SIZE_T n;
            if (!WriteProcessMemory(hProcess,(LPVOID)g_RunData.RetPtr,&pi,sizeof(PROCESS_INFORMATION),&n))
              DBGTrace2("AutoSuRun(%s) WriteProcessMemory failed: %s",
              g_RunData.cmdLine,GetLastErrorNameStatic());
            CloseHandle(hProcess);
          }else
            DBGTrace2("AutoSuRun(%s) OpenProcess failed: %s",
            g_RunData.cmdLine,GetLastErrorNameStatic());
        }
        if (g_RunData.bShlExHook)
          ShowTrayWarning(CBigResStr(IDS_STARTED,BeautifyCmdLine(g_RunData.cmdLine)),IDI_SHIELD,20000);
      }else
        DBGTrace1("CreateProcessAsUser failed: %s",GetLastErrorNameStatic());
      if(bIsExplorer && (!orgSP))
        SetSeparateProcess((HKEY)ProfInf.hProfile,0);
      DestroyEnvironmentBlock(Env);
    }else
      DBGTrace1("CreateEnvironmentBlock failed: %s",GetLastErrorNameStatic());
    UnloadUserProfile(hAdmin,ProfInf.hProfile);
  }
  CloseHandle(hAdmin);
  return RetVal;
}

DWORD DirectStartUserProcess(DWORD ProcId,LPTSTR cmd) 
{
  DWORD RetVal=RETVAL_ACCESSDENIED;
  HANDLE hUser=GetProcessUserToken(ProcId);
  PROFILEINFO ProfInf = {sizeof(ProfInf),0,g_RunData.UserName};
  if(LoadUserProfile(hUser,&ProfInf))
  {
    void* Env=0;
    if (CreateEnvironmentBlock(&Env,hUser,FALSE))
    {
      STARTUPINFO si={0};
      PROCESS_INFORMATION pi={0};
      si.cb	= sizeof(si);
      //Do not inherit Desktop from calling process, use Tokens Desktop
      TCHAR WinstaDesk[MAX_PATH];
      _stprintf(WinstaDesk,_T("%s\\%s"),g_RunData.WinSta,g_RunData.Desk);
      si.lpDesktop = WinstaDesk;
      //CreateProcessAsUser will only work from an NT System Account since the
      //Privilege SE_ASSIGNPRIMARYTOKEN_NAME is not present elsewhere
      EnablePrivilege(SE_ASSIGNPRIMARYTOKEN_NAME);
      EnablePrivilege(SE_INCREASE_QUOTA_NAME);
      if (CreateProcessAsUser(hUser,NULL,cmd,NULL,NULL,FALSE,
            CREATE_UNICODE_ENVIRONMENT,Env,g_RunData.CurDir,&si,&pi))
      {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        RetVal=RETVAL_OK;
        //ShellExec-Hook: We must return the PID and TID to fake CreateProcess:
        if((g_RunData.RetPID)&&(g_RunData.RetPtr))
        {
          pi.hThread=0;
          pi.hProcess=0;
          HANDLE hProcess=OpenProcess(PROCESS_VM_OPERATION|PROCESS_VM_WRITE,FALSE,g_RunData.RetPID);
          if (hProcess)
          {
            SIZE_T n;
            if (!WriteProcessMemory(hProcess,(LPVOID)g_RunData.RetPtr,&pi,sizeof(PROCESS_INFORMATION),&n))
              DBGTrace2("AutoSuRun(%s) WriteProcessMemory failed: %s",
              cmd,GetLastErrorNameStatic());
            CloseHandle(hProcess);
          }else
            DBGTrace2("AutoSuRun(%s) OpenProcess failed: %s",
            cmd,GetLastErrorNameStatic());
        }
      }else
        DBGTrace1("CreateProcess failed: %s",GetLastErrorNameStatic());
      DestroyEnvironmentBlock(Env);
    }
    UnloadUserProfile(hUser,ProfInf.hProfile);
  }
  CloseHandle(hUser);
  return RetVal;
}

//////////////////////////////////////////////////////////////////////////////
// 
//  SuRun
// 
//////////////////////////////////////////////////////////////////////////////
void SuRun(DWORD ProcessID)
{
  //This is called from a separate process created by the service
  if (!IsLocalSystem())
    return;
  zero(g_RunData);
  zero(g_RunPwd);
  RUNDATA RD={0};
  RD.CliProcessId=ProcessID;
  if(CheckCliProcess(RD)!=1)
    return;
  DWORD RetVal=RETVAL_ACCESSDENIED;
  //RunAs...
  if (g_RunData.bRunAs)
  {
    if (CreateSafeDesktop(g_RunData.WinSta,g_RunData.Desk,GetBlurDesk,GetFadeDesk))
    {
      if (!RunAsLogon(g_RunData.SessionID,g_RunData.UserName,g_RunPwd,IDS_ASKRUNAS,BeautifyCmdLine(g_RunData.cmdLine)))
      {
        DeleteSafeDesktop(GetFadeDesk);
        ResumeClient(RETVAL_CANCELLED);
        return;
      }
      RetVal=RETVAL_OK;
      DeleteSafeDesktop(false);
    }else
    {
      ResumeClient(RETVAL_CANCELLED);
      SafeMsgBox(0,CBigResStr(IDS_NODESK),CResStr(IDS_APPNAME),MB_ICONSTOP|MB_SERVICE_NOTIFICATION);
      return;
    }
  }else //if (!g_RunData.bRunAs)
  {
    //Setup?
    if (_tcsicmp(g_RunData.cmdLine,_T("/SETUP"))==0)
    {
      //Create the new desktop
      ResumeClient(RETVAL_OK);
      //check if SuRun Setup is hidden for user name
      if (HideSuRun(g_RunData.UserName,g_RunData.Groups))
        return;
      if (CreateSafeDesktop(g_RunData.WinSta,g_RunData.Desk,GetBlurDesk,GetFadeDesk))
      {
        __try
        {
          Setup();
        }__except(1)
        {
          DBGTrace("FATAL: Exception in Setup()");
        }
        DeleteSafeDesktop(GetFadeDesk);
      }else
        SafeMsgBox(0,CBigResStr(IDS_NODESK),CResStr(IDS_APPNAME),MB_ICONSTOP|MB_SERVICE_NOTIFICATION);
      return;
    }
    if (g_CliIsAdmin && (GetNoConvAdmin||GetNoConvUser))
    {
      //Just start the client process!
      KillProcess(g_RunData.KillPID);
      ResumeClient(DirectStartUserProcess(g_RunData.CliProcessId,g_RunData.cmdLine));
      return;
    }
    //Start execution
    RetVal=PrepareSuRun();
    if (RetVal!=RETVAL_OK)
    {
      if (RetVal==RETVAL_NODESKTOP)
      {
        ResumeClient(RETVAL_CANCELLED);
        SafeMsgBox(0,CBigResStr(IDS_NODESK),CResStr(IDS_APPNAME),MB_ICONSTOP|MB_SERVICE_NOTIFICATION);
        return;
      }
      if ( g_RunData.bShlExHook
        &&(!IsInWhiteList(g_RunData.UserName,g_RunData.cmdLine,FLAG_SHELLEXEC)))
        ResumeClient(RETVAL_SX_NOTINLIST);//let ShellExecute start the process!
      else
        ResumeClient(RetVal);
      return;
    }
  }//(g_RunData.bRunAs)
  //copy the password to the client
  __try
  {
    KillProcess(g_RunData.KillPID);
    RetVal=LSAStartAdminProcess();
  }__except(1)
  {
    DBGTrace("FATAL: Exception in StartAdminProcessTrampoline()");
  }
  //Clear Password
  ResumeClient(RetVal);
}

//////////////////////////////////////////////////////////////////////////////
// 
//  InstallRegistry
// 
//////////////////////////////////////////////////////////////////////////////
bool g_bKeepRegistry=FALSE;
bool g_bDelSuRunners=TRUE;
bool g_bSR2Admins=FALSE;
HWND g_InstLog=0;
#define InstLog(s)  \
{\
  SendMessage(g_InstLog,LB_SETTOPINDEX,\
    SendMessage(g_InstLog,LB_ADDSTRING,0,(LPARAM)(LPCTSTR)s),0);\
  MsgLoop();\
  Sleep(250);\
}

#define UNINSTL L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SuRun"

#define SHLRUN  L"\\Shell\\SuRun"
#define EXERUN  L"exefile" SHLRUN
#define CMDRUN  L"cmdfile" SHLRUN
#define CPLRUN  L"cplfile" SHLRUN
#define MSCRUN  L"mscfile" SHLRUN
#define BATRUN  L"batfile" SHLRUN
#define MSIPTCH L"Msi.Patch" SHLRUN
#define MSIPKG  L"Msi.Package" SHLRUN
#define REGRUN  L"regfile" SHLRUN
#define CPLREG  L"CLSID\\{21EC2020-3AEA-1069-A2DD-08002B30309D}"  SHLRUN

static void MsgLoop()
{
  MSG msg;
  int count=0;
  while (PeekMessage(&msg,0,0,0,PM_REMOVE) && (count++<100))
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

void InstallRegistry()
{
  TCHAR SuRunExe[4096];
  GetSystemWindowsDirectory(SuRunExe,4096);
  InstLog(CResStr(IDS_ADDUNINST));
  PathAppend(SuRunExe,L"SuRun.exe");
  PathQuoteSpaces(SuRunExe);
  CResStr MenuStr(IDS_MENUSTR);
  CBigResStr DefCmd(L"%s \"%%1\" %%*",SuRunExe);
  //UnInstall
  SetRegStr(HKLM,UNINSTL,L"DisplayName",L"Super User Run (SuRun)");
  SetRegStr(HKLM,UNINSTL,L"UninstallString",CBigResStr(L"%s /UNINSTALL",SuRunExe,SuRunExe));
  SetRegStr(HKLM,UNINSTL,L"DisplayVersion",GetVersionString());
  SetRegStr(HKLM,UNINSTL,L"Publisher",L"Kay Bruns");
  SetRegStr(HKLM,UNINSTL,L"URLInfoAbout",L"http://kay-bruns.de/surun");
  SetRegStr(HKLM,UNINSTL,L"DisplayIcon",SuRunExe);
  if (LOBYTE(LOWORD(GetVersion()))<6)
  {
    //WinLogon Notification
    SetRegInt(HKLM,WINLOGONKEY,L"Asynchronous",0);
    SetRegStr(HKLM,WINLOGONKEY,L"DllName",L"SuRunExt.dll");
    SetRegInt(HKLM,WINLOGONKEY,L"Impersonate",0);
    SetRegStr(HKLM,WINLOGONKEY,L"Logoff",L"SuRunLogoffUser");
  }
  //AutoRun, System Menu Hook
  InstLog(CResStr(IDS_ADDAUTORUN));
  SetRegStr(HKLM,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
    CResStr(IDS_SYSMENUEXT),CBigResStr(L"%s /SYSMENUHOOK",SuRunExe));
  if (!g_bKeepRegistry)
  {
    InstLog(CResStr(IDS_ADDASSOC));
    //exefile
    SetRegStr(HKCR,EXERUN,L"",MenuStr);
    SetRegStr(HKCR,EXERUN L"\\command",L"",DefCmd);
    //cmdfile
    SetRegStr(HKCR,CMDRUN,L"",MenuStr);
    SetRegStr(HKCR,CMDRUN L"\\command",L"",DefCmd);
    //cplfile
    SetRegStr(HKCR,CPLRUN,L"",MenuStr);
    SetRegStr(HKCR,CPLRUN L"\\command",L"",DefCmd);
    //MSCFile
    SetRegStr(HKCR,MSCRUN,L"",MenuStr);
    SetRegStr(HKCR,MSCRUN L"\\command",L"",DefCmd);
    //batfile
    SetRegStr(HKCR,BATRUN,L"",MenuStr);
    SetRegStr(HKCR,BATRUN L"\\command",L"",DefCmd);
    //regfile
    SetRegStr(HKCR,REGRUN,L"",MenuStr);
    SetRegStr(HKCR,REGRUN L"\\command",L"",DefCmd);
    TCHAR MSIExe[4096];
    GetSystemDirectory(MSIExe,4096);
    PathAppend(MSIExe,L"msiexec.exe");
    PathQuoteSpaces(MSIExe);
    //MSI Install
    SetRegStr(HKCR,MSIPKG L" open",L"",CResStr(IDS_SURUNINST));
    SetRegStr(HKCR,MSIPKG L" open\\command",L"",CBigResStr(L"%s %s /i \"%%1\" %%*",SuRunExe,MSIExe));
    //MSI Repair
    SetRegStr(HKCR,MSIPKG L" repair",L"",CResStr(IDS_SURUNREPAIR));
    SetRegStr(HKCR,MSIPKG L" repair\\command",L"",CBigResStr(L"%s %s /f \"%%1\" %%*",SuRunExe,MSIExe));
    //MSI Uninstall
    SetRegStr(HKCR,MSIPKG L" Uninstall",L"",CResStr(IDS_SURUNUNINST));
    SetRegStr(HKCR,MSIPKG L" Uninstall\\command",L"",CBigResStr(L"%s %s /x \"%%1\" %%*",SuRunExe,MSIExe));
    //MSP Apply
    SetRegStr(HKCR,MSIPTCH L" open",L"",MenuStr);
    SetRegStr(HKCR,MSIPTCH L" open\\command",L"",CBigResStr(L"%s %s /p \"%%1\" %%*",SuRunExe,MSIExe));
    //Control Panel
    SetRegStr(HKCR,CPLREG,L"",MenuStr);
    SetRegStr(HKCR,CPLREG L"\\command",L"",CBigResStr(L"%s control",SuRunExe));
  }
  //Control Panel Applet
  InstLog(CResStr(IDS_ADDCPL));
  GetSystemWindowsDirectory(SuRunExe,4096);
  PathAppend(SuRunExe,L"SuRunExt.dll");
  SetRegStr(HKLM,L"Software\\Microsoft\\Windows\\CurrentVersion\\Control Panel\\Cpls",L"SuRunCpl",SuRunExe);
  //Add SuRun CPL to "Performance and Maintenance"
  SetRegInt(HKLM,L"Software\\Microsoft\\Windows\\CurrentVersion\\Control Panel\\Extended Properties\\{305CA226-D286-468e-B848-2B2E8E697B74} 2",SuRunExe,5);
}

//////////////////////////////////////////////////////////////////////////////
// 
//  RemoveRegistry
// 
//////////////////////////////////////////////////////////////////////////////
void RemoveRegistry()
{
  if (!g_bKeepRegistry)
  {
    InstLog(CResStr(IDS_REMASSOC));
    //exefile
    DelRegKey(HKCR,EXERUN);
    //cmdfile
    DelRegKey(HKCR,CMDRUN);
    //cplfile
    DelRegKey(HKCR,CPLRUN);
    //MSCFile
    DelRegKey(HKCR,MSCRUN);
    //batfile
    DelRegKey(HKCR,BATRUN);
    //regfile
    DelRegKey(HKCR,REGRUN);
    //MSI Install
    DelRegKey(HKCR,MSIPKG L" open");
    //MSI Repair
    DelRegKey(HKCR,MSIPKG L" repair");
    //MSI Uninstall
    DelRegKey(HKCR,MSIPKG L" Uninstall");
    //MSP Apply
    DelRegKey(HKCR,MSIPTCH L" open");
    //Control Panel
    DelRegKey(HKCR,CPLREG);
  }
  //Control Panel Applet
  InstLog(CResStr(IDS_REMCPL));
  RegDelVal(HKLM,L"Software\\Microsoft\\Windows\\CurrentVersion\\Control Panel\\Cpls",L"SuRunCpl");
  TCHAR CPL[4096];
  GetSystemWindowsDirectory(CPL,4096);
  PathAppend(CPL,L"SuRunExt.dll");
  RegDelVal(HKLM,L"Software\\Microsoft\\Windows\\CurrentVersion\\Control Panel\\Extended Properties\\{305CA226-D286-468e-B848-2B2E8E697B74} 2",CPL);
  //AutoRun, System Menu Hook
  InstLog(CResStr(IDS_REMAUTORUN));
  RegDelVal(HKLM,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",CResStr(IDS_SYSMENUEXT));
  //WinLogon Notification
  if (LOBYTE(LOWORD(GetVersion()))<6)
    DelRegKey(HKLM,WINLOGONKEY);
  //UnInstall
  InstLog(CResStr(IDS_REMUNINST));
  DelRegKey(HKLM,UNINSTL);
}

//////////////////////////////////////////////////////////////////////////////
// 
//  Service control helpers
// 
//////////////////////////////////////////////////////////////////////////////

#define WaitFor(a) \
  {\
    for (int i=0;i<30;i++)\
    {\
      Sleep(750);\
      if (a)\
        return TRUE; \
    } \
    return a;\
  }


BOOL RunThisAsAdmin(LPCTSTR cmd,DWORD WaitStat,int nResId)
{
  TCHAR ModName[MAX_PATH];
  TCHAR cmdLine[4096];
  GetModuleFileName(NULL,ModName,MAX_PATH);
  NetworkPathToUNCPath(ModName);
  PathQuoteSpaces(ModName);
  TCHAR User[UNLEN+GNLEN+2]={0};
  GetProcessUserName(GetCurrentProcessId(),User);
  DWORD dwSess=0;
  ProcessIdToSessionId(GetCurrentProcessId(),&dwSess);
//  if (IsInSuRunners(User,dwSess) && (CheckServiceStatus()==SERVICE_RUNNING))
//  {
//    DBGTrace2("RunThisAsAdmin %s is SuRunner: starting %s with SuRun",User,cmd);
//    TCHAR SvcFile[4096];
//    GetSystemWindowsDirectory(SvcFile,4096);
//    PathAppend(SvcFile,_T("SuRun.exe"));
//    PathQuoteSpaces(SvcFile);
//    _stprintf(cmdLine,_T("%s /QUIET %s %s"),SvcFile,ModName,cmd);
//    STARTUPINFO si={0};
//    PROCESS_INFORMATION pi;
//    si.cb = sizeof(si);
//    if (CreateProcess(NULL,cmdLine,NULL,NULL,FALSE,0,NULL,NULL,&si,&pi))
//    {
//      CloseHandle(pi.hThread);
//      DWORD ExitCode=-1;
//      if (WaitForSingleObject(pi.hProcess,INFINITE)==WAIT_OBJECT_0)
//        GetExitCodeProcess(pi.hProcess,&ExitCode);
//      CloseHandle(pi.hProcess);
//      if (ExitCode!=RETVAL_OK)
//        return FALSE;
//      WaitFor(CheckServiceStatus()==WaitStat);
//    }else
//      DBGTrace2("RunThisAsAdmin CreateProcess(%s) failed: %s",
//                cmd,GetLastErrorNameStatic());
//  }
  _stprintf(cmdLine,_T("%s %s"),ModName,cmd);
  if (!RunAsAdmin(cmdLine,nResId))
    return FALSE;
  WaitFor(CheckServiceStatus()==WaitStat);
}

void DelFile(LPCTSTR File)
{
  InstLog(CBigResStr(IDS_DELFILE,File));
  if (PathFileExists(File) && (!DeleteFile(File)))
  {
    CBigResStr tmp(_T("%s.tmp"),File);
    while (_trename(File,tmp))
      _tcscat(tmp,L".tmp");
    InstLog(CBigResStr(IDS_DELFILEBOOT,(LPCTSTR)tmp));
    MoveFileEx(tmp,NULL,MOVEFILE_DELAY_UNTIL_REBOOT); 
  }
}

void CopyToWinDir(LPCTSTR File)
{
  TCHAR DstFile[4096];
  TCHAR SrcFile[4096];
  GetModuleFileName(NULL,SrcFile,MAX_PATH);
  NetworkPathToUNCPath(SrcFile);
  PathRemoveFileSpec(SrcFile);
  PathAppend(SrcFile,File);
  GetSystemWindowsDirectory(DstFile,4096);
  PathAppend(DstFile,File);
  DelFile(DstFile);
  InstLog(CBigResStr(IDS_COPYFILE,SrcFile,DstFile));
  CopyFile(SrcFile,DstFile,FALSE);
}

BOOL DeleteService(BOOL bJustStop=FALSE)
{
  BOOL bRet=FALSE;
  InstLog(CResStr(IDS_DELSERVICE));
  SC_HANDLE hdlSCM = OpenSCManager(0,0,SC_MANAGER_CONNECT);
  if (hdlSCM) 
  {
    SC_HANDLE hdlServ=OpenService(hdlSCM,SvcName,SERVICE_STOP|DELETE);
    if (hdlServ)
    {
      SERVICE_STATUS ss;
      ControlService(hdlServ,SERVICE_CONTROL_STOP,&ss);
      DeleteService(hdlServ);
      CloseServiceHandle(hdlServ);
      bRet=TRUE;
    }
  }
  for (int n=0;CheckServiceStatus() && (n<100);n++)
    Sleep(100);
  //Delete Files and directories
  TCHAR File[4096];
  GetSystemWindowsDirectory(File,4096);
  PathAppend(File,_T("SuRun.exe"));
  DelFile(File);
  GetSystemWindowsDirectory(File,4096);
  PathAppend(File,_T("SuRunExt.dll"));
  DelFile(File);
#ifdef _WIN64
  GetSystemWindowsDirectory(File,4096);
  PathAppend(File,_T("SuRun32.bin"));
  DelFile(File);
  GetSystemWindowsDirectory(File,4096);
  PathAppend(File,_T("SuRunExt32.dll"));
  DelFile(File);
#endif _WIN64
  //Start Menu
  TCHAR file[4096];
  GetRegStr(HKLM,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
    L"Common Programs",file,4096);
  ExpandEnvironmentStrings(file,File,4096);
  PathAppend(File,CResStr(IDS_STARTMENUDIR));
  DeleteDirectory(File);
  //Registry
  RemoveRegistry();
  if (bJustStop)
    return TRUE;
  //restore RunAs
  ReplaceSuRunWithRunAs();
  //Restore Windows Options
  if (SRGetWinUpdBoot && GetWinUpdBoot)
    SetWinUpdBoot(false);
  if (SRGetWinUpd4All && GetWinUpd4All)
    SetWinUpd4All(false);
  if (SRGetOwnerAdminGrp && GetOwnerAdminGrp)
    SetOwnerAdminGrp(false);
  SetSetEnergy(false);
  SetSetTime(SURUNNERSGROUP,false);
  if (!g_bKeepRegistry)
  {
    InstLog(CResStr(IDS_DELREG));
    //remove COM Object Settings
    DelRegKey(HKCR,L"CLSID\\" sGUID);
    //Remove SuRunners from Registry
    //HKLM\Security\SuRun
    SetRegistryTreeAccess(_T("MACHINE\\") SVCKEY,DOMAIN_ALIAS_RID_ADMINS,true);
    DelRegKey(HKLM,SVCKEY);
    //HKLM\Software\SuRun
    DelRegKey(HKLM,SURUNKEY);
  }
  //Delete "SuRunners"?
  if(g_bDelSuRunners)
  {
    //Make "SuRunners"->"Administrators"?
    if (g_bSR2Admins)
    {
      USERLIST SuRunners;
      SuRunners.SetSurunnersUsers(0,-1,FALSE);
      for (int i=0;i<SuRunners.GetCount();i++)
      {
        InstLog(CResStr(IDS_SR2ADMIN,SuRunners.GetUserName(i)));
        AlterGroupMember(DOMAIN_ALIAS_RID_ADMINS,SuRunners.GetUserName(i),1);
      }
    }
    InstLog(CResStr(IDS_DELSURUNNERS));
    DeleteSuRunnersGroup();
  }
  return bRet;
}

BOOL InstallService()
{
  if (!IsAdmin())
    return RunThisAsAdmin(_T("/INSTALL"),SERVICE_RUNNING,IDS_INSTALLADMIN);
  if (CheckServiceStatus())
  {
    if (!DeleteService(true))
      return FALSE;
    //Wait until "SuRun /SYSMENUHOOK" has exited:
    CTimeOut t(11000);
    for(;;)
    {
      if(t.TimedOut())
        break;
      HANDLE m=CreateMutex(NULL,true,_T("SuRun_SysMenuHookIsRunning"));
      if (GetLastError()!=ERROR_ALREADY_EXISTS)
      {
        CloseHandle(m);
        break;
      }
      CloseHandle(m);
      Sleep(200);
    }
  }
  SC_HANDLE hdlSCM=OpenSCManager(0,0,SC_MANAGER_CREATE_SERVICE);
  if (hdlSCM==0) 
    return FALSE;
  CopyToWinDir(_T("SuRun.exe"));
  CopyToWinDir(_T("SuRunExt.dll"));
#ifdef _WIN64
  CopyToWinDir(_T("SuRun32.bin"));
  CopyToWinDir(_T("SuRunExt32.dll"));
#endif _WIN64
  InstLog(CResStr(IDS_INSTSVC));
  TCHAR SvcFile[4096];
  GetSystemWindowsDirectory(SvcFile,4096);
  PathAppend(SvcFile,_T("SuRun.exe"));
  PathQuoteSpaces(SvcFile);
  _tcscat(SvcFile,_T(" /ServiceRun"));
  SC_HANDLE hdlServ = CreateService(hdlSCM,SvcName,SvcName,STANDARD_RIGHTS_REQUIRED,
                          SERVICE_WIN32_OWN_PROCESS,SERVICE_AUTO_START,
                          SERVICE_ERROR_NORMAL,SvcFile,L"PlugPlay",0,0,0,0);
  if (!hdlServ) 
    return CloseServiceHandle(hdlSCM),FALSE;
  CloseServiceHandle(hdlServ);
  InstLog(CResStr(IDS_STARTSVC));
  hdlServ = OpenService(hdlSCM,SvcName,SERVICE_START);
  BOOL bRet=StartService(hdlServ,0,0);
  if (!bRet)
  {
    CloseServiceHandle(hdlServ);
    hdlServ=OpenService(hdlSCM,SvcName,SERVICE_STOP|DELETE);
    if (hdlServ)
      DeleteService(hdlServ);
  }
  CloseServiceHandle(hdlServ);
  CloseServiceHandle(hdlSCM);
  if (bRet)
  {
    //Registry
    InstallRegistry();
    //"SuRunners" Group
    CreateSuRunnersGroup();
    WaitFor(CheckServiceStatus()==SERVICE_RUNNING);
  }
  return bRet;
}

//////////////////////////////////////////////////////////////////////////////
// 
// Install/Update Dialogs Proc
// 
//////////////////////////////////////////////////////////////////////////////
INT_PTR CALLBACK InstallDlgProc(HWND hwnd,UINT msg,WPARAM wParam,LPARAM lParam)
{
  switch(msg)
  {
  case WM_INITDIALOG:
    {
      SendMessage(hwnd,WM_SETICON,ICON_BIG,
        (LPARAM)LoadImage(GetModuleHandle(0),MAKEINTRESOURCE(IDI_MAINICON),
        IMAGE_ICON,32,32,0));
      SendMessage(hwnd,WM_SETICON,ICON_SMALL,
        (LPARAM)LoadImage(GetModuleHandle(0),MAKEINTRESOURCE(IDI_MAINICON),
        IMAGE_ICON,16,16,0));
      {
        TCHAR WndText[MAX_PATH]={0},newText[MAX_PATH]={0};
        GetWindowText(hwnd,WndText,MAX_PATH);
        _stprintf(newText,WndText,GetVersionString());
        SetWindowText(hwnd,newText);
      }
      SendDlgItemMessage(hwnd,IDC_QUESTION,WM_SETFONT,
        (WPARAM)CreateFont(-24,0,0,0,FW_BOLD,0,0,0,0,0,0,0,0,_T("MS Shell Dlg")),1);
      CheckDlgButton(hwnd,IDC_RUNSETUP,1);
      CheckDlgButton(hwnd,IDC_KEEPREGISTRY,g_bKeepRegistry);
      CheckDlgButton(hwnd,IDC_OWNERGROUP,1);
      if (GetOwnerAdminGrp)
      {
        ShowWindow(GetDlgItem(hwnd,IDC_OWNERGROUP),SW_HIDE);
        ShowWindow(GetDlgItem(hwnd,IDC_OWNGRPST),SW_HIDE);
      }
      CheckDlgButton(hwnd,IDC_DELSURUNNERS,g_bDelSuRunners);
      CheckDlgButton(hwnd,IDC_SR2ADMIN,g_bSR2Admins);
      g_InstLog=GetDlgItem(hwnd,IDC_INSTLOG);
      return FALSE;
    }//WM_INITDIALOG
  case WM_DESTROY:
    {
      HGDIOBJ fs=GetStockObject(DEFAULT_GUI_FONT);
      HGDIOBJ f=(HGDIOBJ)SendDlgItemMessage(hwnd,IDC_QUESTION,WM_GETFONT,0,0);
      SendDlgItemMessage(hwnd,IDC_QUESTION,WM_SETFONT,(WPARAM)fs,0);
      if(f)
        DeleteObject(f);
      return TRUE;
    }
  case WM_CTLCOLORSTATIC:
    {
      int CtlId=GetDlgCtrlID((HWND)lParam);
      if ((CtlId==IDC_QUESTION)||(CtlId==IDC_SECICON))
      {
        SetBkMode((HDC)wParam,TRANSPARENT);
        return (BOOL)PtrToUlong(GetStockObject(WHITE_BRUSH));
      }
    }
    break;
  case WM_NCDESTROY:
    {
      DestroyIcon((HICON)SendMessage(hwnd,WM_GETICON,ICON_BIG,0));
      DestroyIcon((HICON)SendMessage(hwnd,WM_GETICON,ICON_SMALL,0));
      return TRUE;
    }//WM_NCDESTROY
  case WM_COMMAND:
    {
      switch (wParam)
      {
      case MAKELPARAM(IDOK,BN_CLICKED): //Install SuRun:
        {
          //Settings
          g_bKeepRegistry=IsDlgButtonChecked(hwnd,IDC_KEEPREGISTRY)!=0;
          if (IsDlgButtonChecked(hwnd,IDC_OWNERGROUP))
            SetOwnerAdminGrp(1);
          //Hide Checkboxes, show Listbox
          ShowWindow(GetDlgItem(hwnd,IDC_RUNSETUP),SW_HIDE);
          ShowWindow(GetDlgItem(hwnd,IDC_KEEPREGST),SW_HIDE);
          ShowWindow(GetDlgItem(hwnd,IDC_KEEPREGISTRY),SW_HIDE);
          ShowWindow(GetDlgItem(hwnd,IDC_OWNERGROUP),SW_HIDE);
          ShowWindow(GetDlgItem(hwnd,IDC_OWNGRPST),SW_HIDE);
          ShowWindow(GetDlgItem(hwnd,IDC_OPTNST),SW_HIDE);
          ShowWindow(g_InstLog,SW_SHOW);
          //Disable Buttons
          EnableWindow(GetDlgItem(hwnd,IDOK),0);
          EnableWindow(GetDlgItem(hwnd,IDCANCEL),0);
          //Show some Progress
          SetDlgItemText(hwnd,IDC_QUESTION,CResStr(IDC_INSTALL));
          MsgLoop();
          if (!InstallService())
          {
            //Install failed:
            InstLog(CResStr(IDS_INSTFAILED));
            SetDlgItemText(hwnd,IDC_QUESTION,CResStr(IDS_INSTFAILED));
            //Make IDOK->Close, hide IDCANCEL
            SetDlgItemText(hwnd,IDOK,CResStr(IDS_CLOSE));
            ShowWindow(GetDlgItem(hwnd,IDCANCEL),SW_HIDE);
            EnableWindow(GetDlgItem(hwnd,IDOK),1);
            SetWindowLongPtr(GetDlgItem(hwnd,IDOK),GWL_ID,IDCLOSE);
            return TRUE;
          }
          //Run Setup?
          if (IsDlgButtonChecked(hwnd,IDC_RUNSETUP)!=0)
          {
            TCHAR SuRunExe[4096];
            GetSystemWindowsDirectory(SuRunExe,4096);
            PathAppend(SuRunExe,L"SuRun.exe /SETUP");
            PROCESS_INFORMATION pi={0};
            STARTUPINFO si={0};
            si.cb	= sizeof(si);
            if (CreateProcess(NULL,(LPTSTR)SuRunExe,NULL,NULL,FALSE,NORMAL_PRIORITY_CLASS,NULL,NULL,&si,&pi))
            {
              CloseHandle(pi.hThread);
              WaitForSingleObject(pi.hProcess,INFINITE);
              CloseHandle(pi.hProcess);
            }
          }
          //Show success
          SetDlgItemText(hwnd,IDC_QUESTION,CResStr(IDS_INSTSUCCESS));
          //Show "need logoff"
          InstLog(_T(" "));
          if (LOBYTE(LOWORD(GetVersion()))<6)
            InstLog(CResStr(IDS_INSTSUCCESS3))
          else
            InstLog(CResStr(IDS_INSTSUCCESS2));
          //Enable OK, CANCEL
          EnableWindow(GetDlgItem(hwnd,IDOK),1);
          EnableWindow(GetDlgItem(hwnd,IDCANCEL),1);
          //Cancel->Close; OK->Logoff
          OSVERSIONINFO oie;
          oie.dwOSVersionInfoSize=sizeof(oie);
          if (LOBYTE(LOWORD(GetVersion()))<6)
          {
            //2k/XP Reboot required for WinLogon Notification
            SetDlgItemText(hwnd,IDOK,CResStr(IDS_REBOOT));
            ShowWindow(GetDlgItem(hwnd,IDCANCEL),SW_HIDE);
            EnableWindow(GetDlgItem(hwnd,IDOK),1);
            SetWindowLongPtr(GetDlgItem(hwnd,IDOK),GWL_ID,IDCONTINUE);
            SetFocus(GetDlgItem(hwnd,IDOK));
          }else 
          {
            //Vista++ display LogOff 
            SetDlgItemText(hwnd,IDCANCEL,CResStr(IDS_CLOSE));
            SetWindowLongPtr(GetDlgItem(hwnd,IDCANCEL),GWL_ID,IDCLOSE);
            SetDlgItemText(hwnd,IDOK,CResStr(IDC_LOGOFF));
            SetWindowLongPtr(GetDlgItem(hwnd,IDOK),GWL_ID,IDCONTINUE);
            SetFocus(GetDlgItem(hwnd,IDOK));
          }
          return TRUE;
        }
      case MAKELPARAM(IDCANCEL,BN_CLICKED): //Close Dlg
        EndDialog(hwnd,IDCANCEL);
        return TRUE;
      case MAKELPARAM(IDCLOSE,BN_CLICKED): //Close Dlg
        EndDialog(hwnd,IDCLOSE);
        return TRUE;
      case MAKELPARAM(IDCONTINUE,BN_CLICKED): //LogOff
        //ExitWindowsEx will not work here because we run as Admin
        if (LOBYTE(LOWORD(GetVersion()))<6)
        {
          //2k/XP Reboot required for WinLogon Notification
          //ExitWindowsEx(EWX_REBOOT|EWX_FORCE,SHTDN_REASON_MINOR_RECONFIG);
          EnablePrivilege(SE_SHUTDOWN_NAME);
          InitiateSystemShutdown(0,0,0,true,true);
        }
        else
          //Vista++ no WinLogon Notification, just LogOff
          WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE,WTS_CURRENT_SESSION,0);
        EndDialog(hwnd,IDCONTINUE);
        return TRUE;
      case MAKELPARAM(IDIGNORE,BN_CLICKED): //Remove SuRun:
        {
          //Settings
          g_bKeepRegistry=(IsDlgButtonChecked(hwnd,IDC_KEEPREGISTRY)!=0);
          if (GetDlgItem(hwnd,IDC_DELSURUNNERS)) //Uninstall:
          {
            g_bDelSuRunners=(!g_bKeepRegistry) && (IsDlgButtonChecked(hwnd,IDC_DELSURUNNERS)!=0);
            g_bSR2Admins=g_bDelSuRunners && (IsDlgButtonChecked(hwnd,IDC_SR2ADMIN)!=0);
          }else //Update:
          {
            g_bDelSuRunners=FALSE;
            g_bSR2Admins=FALSE;
          }
          //Hide Checkboxes, show Listbox
          ShowWindow(GetDlgItem(hwnd,IDC_KEEPREGISTRY),SW_HIDE);
          ShowWindow(GetDlgItem(hwnd,IDC_DELSURUNNERS),SW_HIDE);
          ShowWindow(GetDlgItem(hwnd,IDC_SR2ADMIN),SW_HIDE);
          ShowWindow(GetDlgItem(hwnd,IDC_OPTNST),SW_HIDE);
          ShowWindow(g_InstLog,SW_SHOW);
          //Disable Buttons
          EnableWindow(GetDlgItem(hwnd,IDIGNORE),0);
          EnableWindow(GetDlgItem(hwnd,IDCANCEL),0);
          //Show some Progress
          SetDlgItemText(hwnd,IDC_QUESTION,CResStr(IDC_UNINSTALL));
          //Make IDIGNORE->Close, hide IDCANCEL
          SetDlgItemText(hwnd,IDIGNORE,CResStr(IDS_CLOSE));
          SetWindowLongPtr(GetDlgItem(hwnd,IDIGNORE),GWL_ID,IDCLOSE);
          ShowWindow(GetDlgItem(hwnd,IDCANCEL),SW_HIDE);
          MsgLoop();
          if (!DeleteService(FALSE))
          {
            //Install failed:
            InstLog(CResStr(IDS_UNINSTFAILED));
            SetDlgItemText(hwnd,IDC_QUESTION,CResStr(IDS_UNINSTFAILED));
            //Enable CLOSE
            EnableWindow(GetDlgItem(hwnd,IDCLOSE),1);
            return TRUE;
          }
          //Show success
          SetDlgItemText(hwnd,IDC_QUESTION,CResStr(IDS_UNINSTSUCCESS));
          InstLog(_T(" "));
          InstLog(CResStr(IDS_UNINSTSUCCESS));
          //Enable CLOSE
          EnableWindow(GetDlgItem(hwnd,IDCLOSE),1);
          return TRUE;
        }
      case MAKELPARAM(IDC_KEEPREGISTRY,BN_CLICKED):
        g_bKeepRegistry=IsDlgButtonChecked(hwnd,IDC_KEEPREGISTRY)!=0;
        EnableWindow(GetDlgItem(hwnd,IDC_DELSURUNNERS),!g_bKeepRegistry);
        //fall through:
      case MAKELPARAM(IDC_DELSURUNNERS,BN_CLICKED):
        EnableWindow(GetDlgItem(hwnd,IDC_SR2ADMIN),
          IsDlgButtonChecked(hwnd,IDC_DELSURUNNERS) &&(!g_bKeepRegistry));
        return TRUE;
      }//switch (wParam)
      break;
    }//WM_COMMAND
  }
  return FALSE;
}

BOOL UserInstall()
{
  if (!IsAdmin())
  {
    DBGTrace("UserInstall: No Admin! starting SuRun /USERINST as Admin");
    return RunThisAsAdmin(_T("/USERINST"),SERVICE_RUNNING,IDS_INSTALLADMIN);
  }
  return DialogBox(GetModuleHandle(0),
      MAKEINTRESOURCE((CheckServiceStatus()!=0)?IDD_UPDATE:IDD_INSTALL),
      0,InstallDlgProc)!=IDCANCEL;
}

BOOL UserUninstall()
{
  if (!IsAdmin())
    return RunThisAsAdmin(_T("/UNINSTALL"),0,IDS_UNINSTALLADMIN);
  return DialogBox(GetModuleHandle(0),
    MAKEINTRESOURCE(IDD_UNINSTALL),0,InstallDlgProc)!=IDCANCEL;
}

//////////////////////////////////////////////////////////////////////////////
// 
// HandleServiceStuff: called on App Entry before WinMain()
// 
//////////////////////////////////////////////////////////////////////////////
bool HandleServiceStuff()
{
  INITCOMMONCONTROLSEX icce={sizeof(icce),ICC_USEREX_CLASSES|ICC_WIN95_CLASSES};
  InitCommonControlsEx(&icce);
#ifdef _DEBUG_ENU
  SetThreadLocale(MAKELCID(MAKELANGID(LANG_ENGLISH,SUBLANG_ENGLISH_US),SORT_DEFAULT));
#endif _DEBUG_ENU
  CCmdLine cmd(0);
  if ((cmd.argc()==3)&&(_tcsicmp(cmd.argv(1),_T("/AskPID"))==0))
  {
    SuRun(wcstol(cmd.argv(2),0,10));
    DeleteSafeDesktop(false);
    ExitProcess(~GetCurrentProcessId());
    return true;
  }
  if (cmd.argc()==2)
  {
    //Service
    if (_tcsicmp(cmd.argv(1),_T("/SERVICERUN"))==0)
    {
      if (!IsLocalSystem())
        return false;
      //Shell Extension
      InstallShellExt();
      SERVICE_TABLE_ENTRY DispatchTable[]={{SvcName,ServiceMain},{0,0}};
      //StartServiceCtrlDispatcher is a blocking call
      StartServiceCtrlDispatcher(DispatchTable);
      //Shell Extension
      RemoveShellExt();
      ExitProcess(0);
      return true;
    }
    //System Menu Hook: This is AutoRun for every user
    if (_tcsicmp(cmd.argv(1),_T("/SYSMENUHOOK"))==0)
    {
#ifdef _WIN64
      CreateMutex(NULL,true,_T("SuRun64_SysMenuHookIsRunning"));
#else _WIN64
      CreateMutex(NULL,true,_T("SuRun_SysMenuHookIsRunning"));
#endif _WIN64
      if (GetLastError()==ERROR_ALREADY_EXISTS)
        return ExitProcess(-1),true;
      g_RunData.CliProcessId=GetCurrentProcessId();
      g_RunData.CliThreadId=GetCurrentThreadId();
      GetWinStaName(g_RunData.WinSta,countof(g_RunData.WinSta));
      GetDesktopName(g_RunData.Desk,countof(g_RunData.Desk));
      GetProcessUserName(g_RunData.CliProcessId,g_RunData.UserName);
      ProcessIdToSessionId(g_RunData.CliProcessId,&g_RunData.SessionID);
      g_RunData.Groups=IsInSuRunnersOrAdmins(g_RunData.UserName,g_RunData.SessionID);
      //ToDo: EnumProcesses,EnumProcessModules,GetModuleFileNameEx to check
      //if the hooks are still loaded

      //In the first three Minutes after Sytstem start:
      //Wait for the service to start
      DWORD ss=CheckServiceStatus();
      if ((ss==SERVICE_STOPPED)||(ss==SERVICE_START_PENDING))
        while ((GetTickCount()<3*60*1000)&&(CheckServiceStatus()!=SERVICE_RUNNING))
            Sleep(1000);
#ifndef _SR32
      StartTSAThread();
#endif _SR32
      if ((!ShowTray(g_RunData.UserName,g_CliIsInAdmins,g_CliIsInSuRunners))
        && IsAdmin())
        return ExitProcess(0),true;
      if ( (!GetUseIATHook) 
        && (!ShowTray(g_RunData.UserName,g_CliIsInAdmins,g_CliIsInSuRunners))
        && (!GetRestartAsAdmin) 
        && (!GetStartAsAdmin))
        return ExitProcess(0),true;
      InstallSysMenuHook();
#ifdef _WIN64
      {
        TCHAR SuRun32Exe[4096];
        GetSystemWindowsDirectory(SuRun32Exe,4096);
        PathAppend(SuRun32Exe,L"SuRun32.bin /SYSMENUHOOK");
        STARTUPINFO si={0};
        PROCESS_INFORMATION pi;
        si.cb = sizeof(si);
        if (CreateProcess(NULL,SuRun32Exe,NULL,NULL,FALSE,0,NULL,NULL,&si,&pi))
        {
          CloseHandle(pi.hProcess);
          CloseHandle(pi.hThread);
        }
      }
#endif _WIN64
      bool TSA=FALSE;
      CTimeOut t(10000);
      for (;;)
      {
        if (t.TimedOut())
        {
          if(CheckServiceStatus()!=SERVICE_RUNNING)
            break;
          g_RunData.Groups=IsInSuRunnersOrAdmins(g_RunData.UserName,g_RunData.SessionID);
          t.Set(10000);
        }
#ifndef _SR32
        if (ShowTray(g_RunData.UserName,g_CliIsInAdmins,g_CliIsInSuRunners))
        {
          if(!TSA)
            InitTrayShowAdmin();
          TSA=TRUE;
          Sleep(ProcessTrayShowAdmin(ShowBalloon(g_RunData.UserName,
            g_CliIsInAdmins,g_CliIsInSuRunners))?55:333);
        }else
#endif _SR32
        {
          if(TSA)
            CloseTrayShowAdmin();
          TSA=FALSE;
          Sleep(1000);
        }
        if ( (!GetUseIATHook) 
          && (!ShowTray(g_RunData.UserName,g_CliIsInAdmins,g_CliIsInSuRunners)) 
          && (!GetRestartAsAdmin) 
          && (!GetStartAsAdmin))
          break;
      }
      if(TSA)
        CloseTrayShowAdmin();
      UninstallSysMenuHook();
      ExitProcess(0);
      return true;
    }
    //Install
    if (_tcsicmp(cmd.argv(1),_T("/INSTALL"))==0)
    {
      InstallService();
      ExitProcess(0);
      return true;
    }
    //UnInstall
    if (_tcsicmp(cmd.argv(1),_T("/UNINSTALL"))==0)
    {
      UserUninstall();
      ExitProcess(0);
      return true;
    }
    //UserInst:
    if (_tcsicmp(cmd.argv(1),_T("/USERINST"))==0)
    {
      UserInstall();
      ExitProcess(0);
      return true;
    }
  }
  if (cmd.argc()==4)
  {
    //WatchDog
    if (_tcsicmp(cmd.argv(1),_T("/WATCHDOG"))==0)
    {
      DoWatchDog(cmd.argv(2),cmd.argv(3));
      ExitProcess(0);
      return true;
    }
  }
  //Are we run from the Windows directory?, if Not, ask for Install/update
  {
    TCHAR fn[4096];
    TCHAR wd[4096];
    GetModuleFileName(NULL,fn,4096);
    NetworkPathToUNCPath(fn);
    PathRemoveFileSpec(fn);
    PathRemoveBackslash(fn);
    GetSystemWindowsDirectory(wd,4096);
    PathRemoveBackslash(wd);
    if(_tcsicmp(fn,wd))
    {
      DBGTrace2("Running from \"%s\" and NOT from WinDir(\"%s\")",fn,wd);
      //Only call UserInstall with empty command line
      if (cmd.argc()!=1)
        ExitProcess(RETVAL_SX_NOTINLIST);
      else
      {
        UserInstall();
        ExitProcess(0);
      }
      return true;
    }
  }
  //In the first three Minutes after Sytstem start:
  //Wait for the service to start
  DWORD ss=CheckServiceStatus();
  if ((ss==SERVICE_STOPPED)||(ss==SERVICE_START_PENDING))
    while ((GetTickCount()<3*60*1000)&&(CheckServiceStatus()!=SERVICE_RUNNING))
        Sleep(1000);
  //The Service must be running!
  ss=CheckServiceStatus();
  if (ss!=SERVICE_RUNNING)
  {
    ExitProcess(-2);//Let ShellExec Hook return
    return true;
  }
  return false;
}
