/* sqUnixMain.c -- support for Unix.
 * 
 *   Copyright (C) 1996-2007 by Ian Piumarta and other authors/contributors
 *                              listed elsewhere in this file.
 *   All rights reserved.
 *   
 *   This file is part of Unix Squeak.
 * 
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the "Software"),
 *   to deal in the Software without restriction, including without limitation
 *   the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *   and/or sell copies of the Software, and to permit persons to whom the
 *   Software is furnished to do so, subject to the following conditions:
 * 
 *   The above copyright notice and this permission notice shall be included in
 *   all copies or substantial portions of the Software.
 * 
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *   DEALINGS IN THE SOFTWARE.
 */

/* Author: Ian Piumarta <ian.piumarta@squeakland.org>
 * Merged with
 *	http://squeakvm.org/svn/squeak/trunk/platforms/unix/vm/sqUnixMain.c
 *	Revision: 2148
 *	Last Changed Rev: 2132
 * by eliot Wed Jan 20 10:57:26 PST 2010
 */

#include "sq.h"
#include "sqMemoryAccess.h"
#include "sqaio.h"
#include "sqUnixCharConv.h"
#include "debug.h"

#ifdef ioMSecs
# undef ioMSecs
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#if !defined(NOEXECINFO)
# include <execinfo.h>
# define BACKTRACE_DEPTH 64
#endif

#if defined(__alpha__) && defined(__osf__)
# include <sys/sysinfo.h>
# include <sys/proc.h>
#endif

#undef	DEBUG_MODULES

#undef	IMAGE_DUMP				/* define to enable SIGHUP and SIGQUIT handling */

#define IMAGE_NAME_SIZE MAXPATHLEN

#define DefaultHeapSize		  20	     	/* megabytes BEYOND actual image size */
#define DefaultMmapSize		1024     	/* megabytes of virtual memory */

       char  *documentName= 0;			/* name if launced from document */
       char   shortImageName[MAXPATHLEN+1];	/* image name */
       char   imageName[MAXPATHLEN+1];		/* full path to image */
static char   vmName[MAXPATHLEN+1];		/* full path to vm */
       char   vmPath[MAXPATHLEN+1];		/* full path to image directory */

       int    argCnt=		0;	/* global copies for access from plugins */
       char **argVec=		0;
       char **envVec=		0;

static int    vmArgCnt=		0;	/* for getAttributeIntoLength() */
static char **vmArgVec=		0;
static int    squeakArgCnt=	0;
static char **squeakArgVec=	0;

static int    extraMemory=	0;
       int    useMmap=		DefaultMmapSize * 1024 * 1024;

static int    useItimer=	1;	/* 0 to disable itimer-based clock */
static int    installHandlers=	1;	/* 0 to disable sigusr1 & sigsegv handlers */
       int    noEvents=		0;	/* 1 to disable new event handling */
       int    noSoundMixer=	0;	/* 1 to disable writing sound mixer levels */
       char  *squeakPlugins=	0;	/* plugin path */
       int    runAsSingleInstance=0;
#if !STACKVM && !COGVM
       int    useJit=		0;	/* use default */
       int    jitProcs=		0;	/* use default */
       int    jitMaxPIC=	0;	/* use default */
#else
# define useJit 0
#endif
       int    withSpy=		0;

       int    uxDropFileCount=	0;	/* number of dropped items	*/
       char **uxDropFileNames=	0;	/* dropped filenames		*/

       int    textEncodingUTF8= 0;	/* 1 if copy from external selection uses UTF8 */

#if defined(IMAGE_DUMP)
static int    dumpImageFile=	0;	/* 1 after SIGHUP received */
#endif

#if defined(DARWIN)
int inModalLoop= 0;
#endif

int sqIgnorePluginErrors	= 0;
int runInterpreter		= 1;

#include "SqDisplay.h"
#include "SqSound.h"

struct SqDisplay *dpy= 0;
struct SqSound   *snd= 0;

extern void dumpPrimTraceLog(void);


/*
 * In the Cog VMs time management is in platforms/unix/vm/sqUnixHeartbeat.c.
 */
#if !STACKVM
/*** timer support ***/

#define	LOW_RES_TICK_MSECS	20	/* 1/50 second resolution */

static unsigned int   lowResMSecs= 0;
static struct timeval startUpTime;

static void sigalrm(int signum)
{
  lowResMSecs+= LOW_RES_TICK_MSECS;
  forceInterruptCheck();
}

static void initTimers(void)
{
  /* set up the micro/millisecond clock */
  gettimeofday(&startUpTime, 0);
  if (useItimer)
    {
      /* set up the low-res (50th second) millisecond clock */
      /* WARNING: all system calls must check for EINTR!!! */
      {
	struct sigaction sa;
	sigset_t ss1, ss2;
	sigemptyset(&ss1);
	sigprocmask(SIG_BLOCK, &ss1, &ss2);
	sa.sa_handler= sigalrm;
	sa.sa_mask= ss2;
#      ifdef SA_RESTART	/* we're probably on Linux */
	sa.sa_flags= SA_RESTART;
#      else
	sa.sa_flags= 0;	/* assume we already have BSD behaviour */
#      endif
#      if defined(__linux__) && !defined(__ia64) &7 !defined(__alpha__)
	sa.sa_restorer= 0;
#      endif
	sigaction(SIGALRM, &sa, 0);
      }
      {
	struct itimerval iv;
	iv.it_interval.tv_sec= 0;
	iv.it_interval.tv_usec= LOW_RES_TICK_MSECS * 1000;
	iv.it_value= iv.it_interval;
	setitimer(ITIMER_REAL, &iv, 0);
      }
    }
}

sqInt ioLowResMSecs(void)
{
  return (useItimer)
    ? lowResMSecs
    : ioMSecs();
}

sqInt ioMSecs(void)
{
  struct timeval now;
  unsigned int nowMSecs;

#if 1 /* HAVE_HIGHRES_COUNTER */

  /* if we have a cheap, high-res counter use that to limit
     the frequency of calls to gettimeofday to something reasonable. */
  static unsigned int baseMSecs = 0;      /* msecs when we took base tick */
  static sqLong baseTicks = 0;/* base tick for adjustment */
  static sqLong tickDelta = 0;/* ticks / msec */
  static sqLong nextTick = 0; /* next tick to check gettimeofday */

  sqLong thisTick = ioHighResClock();

  if(thisTick < nextTick) return lowResMSecs;

#endif

  gettimeofday(&now, 0);
  if ((now.tv_usec-= startUpTime.tv_usec) < 0)
    {
      now.tv_usec+= 1000000;
      now.tv_sec-= 1;
    }
  now.tv_sec-= startUpTime.tv_sec;
  nowMSecs = (now.tv_usec / 1000 + now.tv_sec * 1000);

#if 1 /* HAVE_HIGHRES_COUNTER */
  {
    unsigned int msecsDelta;
    /* Adjust our rdtsc rate every 10...100 msecs as needed.
       This also covers msecs clock-wraparound. */
    msecsDelta = nowMSecs - baseMSecs;
    if(msecsDelta < 0 || msecsDelta > 100) {
      /* Either we've hit a clock-wraparound or we are being
	 sampled in intervals larger than 100msecs.
	 Don't try any fancy adjustments */
      baseMSecs = nowMSecs;
      baseTicks = thisTick;
      nextTick = 0;
      tickDelta = 0;
    } else if(msecsDelta >= 10) {
      /* limit the rate of adjustments to 10msecs */
      baseMSecs = nowMSecs;
      tickDelta = (thisTick - baseTicks) / msecsDelta;
      nextTick = baseTicks = thisTick;
    }
    nextTick += tickDelta;
  }
#endif
  return lowResMSecs= nowMSecs;
}

sqInt ioMicroMSecs(void)
{
  /* return the highest available resolution of the millisecond clock */
  return ioMSecs();	/* this already to the nearest millisecond */
}

time_t convertToSqueakTime(time_t unixTime);

/* returns the local wall clock time */
sqInt ioSeconds(void)
{
  return convertToSqueakTime(time(0));
}
#endif /* STACKVM */

time_t convertToSqueakTime(time_t unixTime)
{
#ifdef HAVE_TM_GMTOFF
  unixTime+= localtime(&unixTime)->tm_gmtoff;
#else
# ifdef HAVE_TIMEZONE
  unixTime+= ((daylight) * 60*60) - timezone;
# else
#  error: cannot determine timezone correction
# endif
#endif
  /* Squeak epoch is Jan 1, 1901.  Unix epoch is Jan 1, 1970: 17 leap years
     and 52 non-leap years later than Squeak. */
  return unixTime + ((52*365UL + 17*366UL) * 24*60*60UL);
}


/*** VM & Image File Naming ***/


/* copy src filename to target, if src is not an absolute filename,
 * prepend the cwd to make target absolute
  */
static void pathCopyAbs(char *target, const char *src, size_t targetSize)
{
  if (src[0] == '/')
    strcpy(target, src);
  else
    {
      0 == getcwd(target, targetSize);
      strcat(target, "/");
      strcat(target, src);
    }
}


static void recordFullPathForVmName(const char *localVmName)
{
#if defined(__linux__)
  char	 name[MAXPATHLEN+1];
  int    len;

  if ((len= readlink("/proc/self/exe", name, sizeof(name))) > 0)
    {
      struct stat st;
      name[len]= '\0';
      if (!stat(name, &st))
	localVmName= name;
    }
#endif

  /* get canonical path to vm */
  if (realpath(localVmName, vmPath) == 0)
    pathCopyAbs(vmPath, localVmName, sizeof(vmPath));

  /* truncate vmPath to dirname */
  {
    int i= 0;
    for (i= strlen(vmPath); i >= 0; i--)
      if ('/' == vmPath[i])
	{
	  vmPath[i+1]= '\0';
	  break;
	}
  }
}

static void recordFullPathForImageName(const char *localImageName)
{
  struct stat s;
  /* get canonical path to image */
  if ((stat(localImageName, &s) == -1) || (realpath(localImageName, imageName) == 0))
    pathCopyAbs(imageName, localImageName, sizeof(imageName));
}

/* vm access */

sqInt imageNameSize(void)
{
  return strlen(imageName);
}

sqInt imageNameGetLength(sqInt sqImageNameIndex, sqInt length)
{
  char *sqImageName= pointerForOop(sqImageNameIndex);
  int count, i;

  count= strlen(imageName);
  count= (length < count) ? length : count;

  /* copy the file name into the Squeak string */
  for (i= 0; i < count; i++)
    sqImageName[i]= imageName[i];

  return count;
}


sqInt imageNamePutLength(sqInt sqImageNameIndex, sqInt length)
{
  char *sqImageName= pointerForOop(sqImageNameIndex);
  int count, i;

  count= (IMAGE_NAME_SIZE < length) ? IMAGE_NAME_SIZE : length;

  /* copy the file name into a null-terminated C string */
  for (i= 0; i < count; i++)
    imageName[i]= sqImageName[i];
  imageName[count]= 0;

  dpy->winSetName(imageName);

  return count;
}


char *getImageName(void)
{
  return imageName;
}


/*** VM Home Directory Path ***/


sqInt vmPathSize(void)
{
  return strlen(vmPath);
}

sqInt vmPathGetLength(sqInt sqVMPathIndex, sqInt length)
{
  char *stVMPath= pointerForOop(sqVMPathIndex);
  int count, i;

  count= strlen(vmPath);
  count= (length < count) ? length : count;

  /* copy the file name into the Squeak string */
  for (i= 0; i < count; i++)
    stVMPath[i]= vmPath[i];

  return count;
}

char* ioGetLogDirectory(void) { return ""; };
sqInt ioSetLogDirectoryOfSize(void* lblIndex, sqInt sz){ return 1; }


/*** power management ***/


sqInt ioDisablePowerManager(sqInt disableIfNonZero)
{
  return true;
}


/*** Access to system attributes and command-line arguments ***/


/* OS_TYPE may be set in configure.in and passed via the Makefile */

#ifndef OS_TYPE
# ifdef UNIX
#   define OS_TYPE "unix"
# else
#  define OS_TYPE "unknown"
# endif
#endif

static char *getAttribute(sqInt id)
{
  if (id < 0)	/* VM argument */
    {
      if (-id  < vmArgCnt)
	return vmArgVec[-id];
    }
  else
    switch (id)
      {
      case 0:
	return vmName[0] ? vmName : vmArgVec[0];
      case 1:
	return imageName;
      case 1001:
	/* OS type: "unix", "win32", "mac", ... */
	return OS_TYPE;
      case 1002:
	/* OS name: "solaris2.5" on unix, "win95" on win32, ... */
	return VM_HOST_OS;
      case 1003:
	/* processor architecture: "68k", "x86", "PowerPC", ...  */
	return VM_HOST_CPU;
      case 1004:
	/* Interpreter version string */
	return  (char *)interpreterVersion;
      case 1005:
	/* window system name */
	return  dpy->winSystemName();
      case 1006:
	/* vm build string */
	return VM_BUILD_STRING;
#if STACKVM
      case 1007: { /* interpreter build info */
	extern char *__interpBuildInfo;
	return __interpBuildInfo;
      }
# if COGVM
      case 1008: { /* cogit build info */
	extern char *__cogitBuildInfo;
	return __cogitBuildInfo;
      }
# endif
#endif
      default:
	if ((id - 2) < squeakArgCnt)
	  return squeakArgVec[id - 2];
      }
  success(false);
  return "";
}

sqInt attributeSize(sqInt id)
{
  return strlen(getAttribute(id));
}

sqInt getAttributeIntoLength(sqInt id, sqInt byteArrayIndex, sqInt length)
{
  if (length > 0)
    strncpy(pointerForOop(byteArrayIndex), getAttribute(id), length);
  return 0;
}


/*** event handling ***/


sqInt inputEventSemaIndex= 0;


/* set asynchronous input event semaphore  */

sqInt ioSetInputSemaphore(sqInt semaIndex)
{
  if ((semaIndex == 0) || (noEvents == 1))
    success(false);
  else
    inputEventSemaIndex= semaIndex;
  return true;
}


/*** display functions ***/

sqInt ioFormPrint(sqInt bitsAddr, sqInt width, sqInt height, sqInt depth, double hScale, double vScale, sqInt landscapeFlag)
{
  return dpy->ioFormPrint(bitsAddr, width, height, depth, hScale, vScale, landscapeFlag);
}

#if STACKVM
sqInt ioRelinquishProcessorForMicroseconds(sqInt us)
{
  dpy->ioRelinquishProcessorForMicroseconds(us);
  return 0;
}
#else /* STACKVM */
static int lastInterruptCheck= 0;

sqInt ioRelinquishProcessorForMicroseconds(sqInt us)
{
  int now;
  dpy->ioRelinquishProcessorForMicroseconds(us);
  now= ioLowResMSecs();
  if (now - lastInterruptCheck > (1000/25))	/* avoid thrashing intr checks from 1ms loop in idle proc  */
    {
      forceInterruptCheck();	/* ensure timely poll for semaphore activity */
      lastInterruptCheck= now;
    }
  return 0;
}
#endif /* STACKVM */

sqInt ioBeep(void)				 { return dpy->ioBeep(); }

#if defined(IMAGE_DUMP)

static void emergencyDump(int quit)
{
  extern sqInt preSnapshot(void);
  extern sqInt postSnapshot(void);
  extern void writeImageFile(sqInt);
  char savedName[MAXPATHLEN];
  char baseName[MAXPATHLEN];
  char *term;
  int  dataSize, i;
  strncpy(savedName, imageName, MAXPATHLEN);
  strncpy(baseName, imageName, MAXPATHLEN);
  if ((term= strrchr(baseName, '.')))
    *term= '\0';
  for (i= 0; ++i;)
    {
      struct stat sb;
      sprintf(imageName, "%s-emergency-dump-%d.image", baseName, i);
      if (stat(imageName, &sb))
	break;
    }
  dataSize= preSnapshot();
  writeImageFile(dataSize);

  printf("\nMost recent primitives\n");
  dumpPrimTraceLog();
  fprintf(stderr, "\n");
  printCallStack();
  fprintf(stderr, "\nTo recover valuable content from this image:\n");
  fprintf(stderr, "    squeak %s\n", imageName);
  fprintf(stderr, "and then evaluate\n");
  fprintf(stderr, "    Smalltalk processStartUpList: true\n");
  fprintf(stderr, "in a workspace.  DESTROY the dumped image after recovering content!");

  if (quit) abort();
  strncpy(imageName, savedName, sizeof(imageName));
}

#endif

sqInt ioProcessEvents(void)
{
#if defined(IMAGE_DUMP)
  if (dumpImageFile)
    {
      emergencyDump(0);
      dumpImageFile= 0;
    }
#endif
  return dpy->ioProcessEvents();
}

sqInt ioScreenDepth(void)		 { return dpy->ioScreenDepth(); }
sqInt ioScreenSize(void)		 { return dpy->ioScreenSize(); }

sqInt ioSetCursorWithMask(sqInt cursorBitsIndex, sqInt cursorMaskIndex, sqInt offsetX, sqInt offsetY)
{
  return dpy->ioSetCursorWithMask(cursorBitsIndex, cursorMaskIndex, offsetX, offsetY);
}

sqInt ioSetCursorARGB(sqInt cursorBitsIndex, sqInt extentX, sqInt extentY, sqInt offsetX, sqInt offsetY)
{
  return dpy->ioSetCursorARGB(cursorBitsIndex, extentX, extentY, offsetX, offsetY);
}

sqInt ioSetCursor(sqInt cursorBitsIndex, sqInt offsetX, sqInt offsetY)
{
  return ioSetCursorWithMask(cursorBitsIndex, 0, offsetX, offsetY);
}

sqInt ioSetFullScreen(sqInt fullScreen)	{ return dpy->ioSetFullScreen(fullScreen); }
sqInt ioForceDisplayUpdate(void)	{ return dpy->ioForceDisplayUpdate(); }

sqInt ioShowDisplay(sqInt dispBitsIndex, sqInt width, sqInt height, sqInt depth, sqInt l, sqInt r, sqInt t, sqInt b)
{
  return dpy->ioShowDisplay(dispBitsIndex, width, height, depth, l, r, t, b);
}

sqInt ioHasDisplayDepth(sqInt i) { return dpy->ioHasDisplayDepth(i); }

sqInt ioSetDisplayMode(sqInt width, sqInt height, sqInt depth, sqInt fullscreenFlag)
{
  return dpy->ioSetDisplayMode(width, height, depth, fullscreenFlag);
}

sqInt clipboardSize(void)
{
  return dpy->clipboardSize();
}

sqInt clipboardWriteFromAt(sqInt count, sqInt byteArrayIndex, sqInt startIndex)
{
  return dpy->clipboardWriteFromAt(count, byteArrayIndex, startIndex);
}

sqInt clipboardReadIntoAt(sqInt count, sqInt byteArrayIndex, sqInt startIndex)
{
  return dpy->clipboardReadIntoAt(count, byteArrayIndex, startIndex);
}

char **clipboardGetTypeNames(void)
{
  return dpy->clipboardGetTypeNames();
}

sqInt clipboardSizeWithType(char *typeName, int ntypeName)
{
  return dpy->clipboardSizeWithType(typeName, ntypeName);
}

void clipboardWriteWithType(char *data, size_t nData, char *typeName, size_t nTypeNames, int isDnd, int isClaiming)
{
  dpy->clipboardWriteWithType(data, nData, typeName, nTypeNames, isDnd, isClaiming);
}

sqInt ioGetButtonState(void)		{ return dpy->ioGetButtonState(); }
sqInt ioPeekKeystroke(void)		{ return dpy->ioPeekKeystroke(); }
sqInt ioGetKeystroke(void)		{ return dpy->ioGetKeystroke(); }
sqInt ioGetNextEvent(sqInputEvent *evt)	{ return dpy->ioGetNextEvent(evt); }
sqInt ioMousePoint(void)		{ return dpy->ioMousePoint(); }

/*** Window labeling ***/
char* ioGetWindowLabel(void) {return "";}

sqInt ioSetWindowLabelOfSize(void* lbl, sqInt size)
{ return dpy->hostWindowSetTitle((long)dpy->ioGetWindowHandle(), lbl, size); }

sqInt ioIsWindowObscured(void) {return false;}

/** Misplaced Window-Size stubs, so the VM will link. **/
sqInt ioGetWindowWidth()
{ int wh = dpy->hostWindowGetSize((long)dpy->ioGetWindowHandle());
  return wh >> 16; } 

sqInt ioGetWindowHeight()
{ int wh = dpy->hostWindowGetSize((long)dpy->ioGetWindowHandle());
  return (short)wh; } 

void* ioGetWindowHandle(void) { return dpy->ioGetWindowHandle(); }

sqInt ioSetWindowWidthHeight(sqInt w, sqInt h)
{ return dpy->hostWindowSetSize((long)dpy->ioGetWindowHandle(),w,h); }

/*** Drag and Drop ***/

sqInt dndOutStart(char *types, int ntypes)	{ return dpy->dndOutStart(types, ntypes); }
sqInt dndOutAcceptedType(char *type, int ntype)	{ return dpy->dndOutAcceptedType(type, ntype); }
void  dndOutSend(char *bytes, int nbytes)	{        dpy->dndOutSend(bytes, nbytes); }
void  dndReceived(char *fileName)			{        dpy->dndReceived(fileName); }
char *dndNameIfActive(char *fileName, int dropIndex) { return dpy->dndNameIfActive(fileName, dropIndex); }

/*** OpenGL ***/

int verboseLevel= 1;

struct SqDisplay *ioGetDisplayModule(void)	{ return dpy; }

void *ioGetDisplay(void)			{ return dpy->ioGetDisplay(); }
void *ioGetWindow(void)				{ return dpy->ioGetWindow(); }
sqInt ioGLinitialise(void)			{ return dpy->ioGLinitialise(); }

sqInt  ioGLcreateRenderer(glRenderer *r, sqInt x, sqInt y, sqInt w, sqInt h, sqInt flags)
{
  return dpy->ioGLcreateRenderer(r, x, y, w, h, flags);
}

sqInt ioGLmakeCurrentRenderer(glRenderer *r)	{ return dpy->ioGLmakeCurrentRenderer(r); }
void  ioGLdestroyRenderer(glRenderer *r)	{	 dpy->ioGLdestroyRenderer(r); }
void  ioGLswapBuffers(glRenderer *r)		{	 dpy->ioGLswapBuffers(r); }

void  ioGLsetBufferRect(glRenderer *r, sqInt x, sqInt y, sqInt w, sqInt h)
{
  dpy->ioGLsetBufferRect(r, x, y, w, h);
}


sqInt  primitivePluginBrowserReady(void)	{ return dpy->primitivePluginBrowserReady(); }
sqInt  primitivePluginRequestURLStream(void)	{ return dpy->primitivePluginRequestURLStream(); }
sqInt  primitivePluginRequestURL(void)		{ return dpy->primitivePluginRequestURL(); }
sqInt  primitivePluginPostURL(void)		{ return dpy->primitivePluginPostURL(); }
sqInt  primitivePluginRequestFileHandle(void)	{ return dpy->primitivePluginRequestFileHandle(); }
sqInt  primitivePluginDestroyRequest(void)	{ return dpy->primitivePluginDestroyRequest(); }
sqInt  primitivePluginRequestState(void)	{ return dpy->primitivePluginRequestState(); }


/*** errors ***/

static void outOfMemory(void)
{
  /* pushing stderr outputs the error report on stderr instead of stdout */
  pushOutputFile((char *)STDERR_FILENO);
  error("out of memory\n");
}

/* Print an error message, possibly a stack trace, do /not/ exit.
 * Allows e.g. writing to a log file and stderr.
 */
static void
reportStackState(char *msg, char *date, int printAll, ucontext_t *uap)
{
#if !defined(NOEXECINFO)
	void *addrs[BACKTRACE_DEPTH];
	int depth;
#endif
	/* flag prevents recursive error when trying to print a broken stack */
	static sqInt printingStack = false;

	printf("\n%s%s%s\n\n", msg, date ? " " : "", date ? date : "");

#if !defined(NOEXECINFO)
	printf("C stack backtrace:\n");
	fflush(stdout); /* backtrace_symbols_fd uses unbuffered i/o */
	depth = backtrace(addrs, BACKTRACE_DEPTH);
	backtrace_symbols_fd(addrs, depth, fileno(stdout));
#endif

	if (ioOSThreadsEqual(ioCurrentOSThread(),getVMOSThread())) {
		if (!printingStack) {
#if COGVM
			/* If we're in generated machine code then the only way the stack
			 * dump machinery has of giving us an accurate report is if we set
			 * stackPointer & framePointer to the native stack & frame pointers.
			 */
# if __APPLE__ && __MACH__ && __i386__
			void *fp = (void *)(uap ? uap->uc_mcontext->ss.ebp: 0);
			void *sp = (void *)(uap ? uap->uc_mcontext->ss.esp: 0);
# elif __linux__ && __i386__
			void *fp = (void *)(uap ? uap->uc_mcontext.gregs[REG_EBP]: 0);
			void *sp = (void *)(uap ? uap->uc_mcontext.gregs[REG_ESP]: 0);
# else
#	error need to implement extracting pc from a ucontext_t on this system
# endif
			char *savedSP, *savedFP;

			ifValidWriteBackStackPointersSaveTo(fp,sp,&savedFP,&savedSP);
#endif

			printingStack = true;
			if (printAll) {
				printf("\n\nAll Smalltalk process stacks (active first):\n");
				printAllStacks();
			}
			else {
				printf("\n\nSmalltalk stack dump:\n");
				printCallStack();
			}
			printingStack = false;
#if COGVM
			/* Now restore framePointer and stackPointer via same function */
			ifValidWriteBackStackPointersSaveTo(savedFP,savedSP,0,0);
#endif
		}
	}
	else
		printf("\nCan't dump Smalltalk stack(s). Not in VM thread\n");
	printf("\nMost recent primitives\n");
	dumpPrimTraceLog();
	fflush(stdout);
}

/* Print an error message, possibly a stack trace, and exit. */
/* Disable Intel compiler inlining of error which is used for breakpoints */
#pragma auto_inline off
void
error(char *msg)
{
	reportStackState(msg,0,0,0);
	abort();
}
#pragma auto_inline on

/* construct /dir/for/image/crash.dmp if a / in imageName else crash.dmp */
static void
getCrashDumpFilenameInto(char *buf)
{
  char *slash;

  strcpy(buf,imageName);
  slash = strrchr(buf,'/');
  strcpy(slash ? slash + 1 : buf, "crash.dmp");
}

static void
sigusr1(int sig, siginfo_t *info, ucontext_t *uap)
{
	int saved_errno = errno;
	time_t now = time(NULL);
	char ctimebuf[32];
	char crashdump[IMAGE_NAME_SIZE+1];
	unsigned long pc;

	if (!ioOSThreadsEqual(ioCurrentOSThread(),getVMOSThread())) {
		pthread_kill(getVMOSThread(),sig);
		errno = saved_errno;
		return;
	}

	getCrashDumpFilenameInto(crashdump);
	ctime_r(&now,ctimebuf);
	pushOutputFile(crashdump);
	reportStackState("SIGUSR1", ctimebuf, 1, uap);
	popOutputFile();
	reportStackState("SIGUSR1", ctimebuf, 1, uap);

	errno = saved_errno;
}

static void
sigsegv(int sig, siginfo_t *info, ucontext_t *uap)
{
	time_t now = time(NULL);
	char ctimebuf[32];
	char crashdump[IMAGE_NAME_SIZE+1];

	getCrashDumpFilenameInto(crashdump);
	ctime_r(&now,ctimebuf);
	pushOutputFile(crashdump);
	reportStackState("Segmentation fault", ctimebuf, 0, uap);
	popOutputFile();
	reportStackState("Segmentation fault", ctimebuf, 0, uap);
	abort();
}



#if defined(IMAGE_DUMP)
static void
sighup(int ignore) { dumpImageFile= 1; }

static void
sigquit(int ignore) { emergencyDump(1); }
#endif


/*** modules ***/


#include "SqModule.h"

struct SqModule *displayModule=	0;
struct SqModule *soundModule=	0;
struct SqModule *modules= 0;

#define modulesDo(M)	for (M= modules;  M;  M= M->next)

struct moduleDescription
{
  struct SqModule **addr;
  char		   *type;
  char		   *name;
};

static struct moduleDescription moduleDescriptions[]=
{
  { &displayModule, "display", "X11"    },	/*** NO DEFAULT ***/
  { &displayModule, "display", "fbdev"  },	/*** NO DEFAULT ***/
  { &displayModule, "display", "null"   },	/*** NO DEFAULT ***/
  { &displayModule, "display", "custom" },	/*** NO DEFAULT ***/
  { &soundModule,   "sound",   "NAS"    },	/*** NO DEFAULT ***/
  { &soundModule,   "sound",   "custom" },	/*** NO DEFAULT ***/
  /* when adding an entry above be sure to change the defaultModules offset below */
  { &displayModule, "display", "Quartz" },	/* defaults... */
  { &soundModule,   "sound",   "OSS"    },
  { &soundModule,   "sound",   "MacOSX" },
  { &soundModule,   "sound",   "Sun"    },
  { &soundModule,   "sound",   "pulse"  },
  { &soundModule,   "sound",   "ALSA"   },
  { &soundModule,   "sound",   "null"   },
  { 0,              0,         0	}
};

static struct moduleDescription *defaultModules= moduleDescriptions + 6;


struct SqModule *queryLoadModule(char *type, char *name, int query)
{
  char modName[MAXPATHLEN], itfName[32];
  struct SqModule *module= 0;
  void *itf= 0;
  sprintf(modName, "vm-%s-%s", type, name);
#ifdef DEBUG_MODULES
  printf("looking for module %s\n", modName);
#endif
  modulesDo (module)
    if (!strcmp(module->name, modName))
      return module;
  sprintf(itfName, "%s_%s", type, name);
  itf= ioFindExternalFunctionIn(itfName, ioLoadModule(0));
  if (!itf)
    {
      void *handle= ioLoadModule(modName);
      if (handle)
	itf= ioFindExternalFunctionIn(itfName, handle);
      else
	if (!query)
	  {
	    fprintf(stderr, "could not find module %s\n", modName);
	    return 0;
	  }
    }
  if (itf)
    {
      module= (struct SqModule *)itf;
      if (SqModuleVersion != module->version)
	{
	  fprintf(stderr, "module %s version %x does not have required version %x\n",
		  modName, module->version, SqModuleVersion);
	  abort();
	}
      module->next= modules;
      modules= module;
      module->name= strdup(modName);
      module->parseEnvironment();
      return module;
    }
  if (!query)
    fprintf(stderr, "could not find interface %s in module %s\n", itfName, modName);
  return 0;
}


struct SqModule *queryModule(char *type, char *name)
{
  return queryLoadModule(type, name, 1);
}

struct SqModule *loadModule(char *type, char *name)
{
  return queryLoadModule(type, name, 0);
}

struct SqModule *requireModule(char *type, char *name)
{
  struct SqModule *m= loadModule(type, name);
  if (!m) abort();
  return m;
}


static char *canonicalModuleName(char *name)
{
  struct moduleDescription *md;

  for (md= moduleDescriptions;  md->addr;  ++md)
    if (!strcasecmp(name, md->name))
      return md->name;
  if (!strcasecmp(name, "none"))
    return "null";
  return name;
}


static void requireModuleNamed(char *type)	/*** NOTE: MODIFIES THE ARGUMENT! ***/
{
  if      (!strncmp(type,  "vm-", 3)) type+= 3;
  else if (!strncmp(type, "-vm-", 4)) type+= 4;
  /* we would like to use strsep() here, but neither OSF1 nor Solaris have it */
  {
    char *name= type;

    while (*name && ('-' != *name) && ('=' != *name))
      ++name;
    if (*name) *name++= '\0';

#  if defined(DEBUG_MODULES)
    printf("type %s name %s\n", type, name);
#  endif
    {
      struct SqModule **addr= 0, *module= 0;

      if      (!strcmp(type, "display")) addr= &displayModule;
      else if (!strcmp(type, "sound"))   addr= &soundModule;
      /* let unknown types through to the following to generate a more informative diagnostic */
      name= canonicalModuleName(name);
      module= requireModule(type, name);
      if (!addr)
	{
	  fprintf(stderr, "this cannot happen\n");
	  abort();
	}
      *addr= module;
    }
  }
}

static void requireModulesNamed(char *specs)
{
  char *vec= strdup(specs);
  char *pos= vec;

  while (*pos)
    {
      char *end= pos;
      while (*end && (' ' <= *end) && (',' != *end))
	++end;
      if (*end) *end++= '\0';
      requireModuleNamed(pos);
      pos= end;
    }
  free(vec);
}


static void checkModuleVersion(struct SqModule *module, int required, int actual)
{
  if (required != actual)
    {
      fprintf(stderr, "module %s interface version %x does not have required version %x\n",
	      module->name, actual, required);
      abort();
    }
}


static void loadImplicit(struct SqModule **addr, char *evar, char *type, char *name)
{
  if ((!*addr) && getenv(evar) && !(*addr= queryModule(type, name)))
    {
      fprintf(stderr, "could not find %s driver vm-%s-%s; either:\n", type, type, name);
      fprintf(stderr, "  - check that %s/vm-%s-%s.so exists, or\n", vmPath, type, name);
      fprintf(stderr, "  - use the '-plugins <path>' option to tell me where it is, or\n");
      fprintf(stderr, "  - remove %s from your environment.\n", evar);
      abort();
    }
}

static void loadModules(void)
{
  loadImplicit(&displayModule, "DISPLAY",     "display", "X11");
  loadImplicit(&soundModule,   "AUDIOSERVER", "sound",   "NAS");
  {
    struct moduleDescription *md;

    for (md= defaultModules;  md->addr;  ++md)
      if (!*md->addr)
	if ((*md->addr= queryModule(md->type, md->name)))
#	 if defined(DEBUG_MODULES)
	  fprintf(stderr, "squeak: %s driver defaulting to vm-%s-%s\n", md->type, md->type, md->name)
#	 endif
	    ;
  }

  if (!displayModule)
    {
      fprintf(stderr, "squeak: could not find any display driver\n");
      abort();
    }
  if (!soundModule)
    {
      fprintf(stderr, "squeak: could not find any sound driver\n");
      abort();
    }

  dpy= (struct SqDisplay *)displayModule->makeInterface();
  snd= (struct SqSound   *)soundModule  ->makeInterface();

  checkModuleVersion(displayModule, SqDisplayVersion, dpy->version);
  checkModuleVersion(soundModule,   SqSoundVersion,   snd->version);
}

/* built-in main vm module */


static int strtobkm(const char *str)
{
  char *suffix;
  int value= strtol(str, &suffix, 10);
  switch (*suffix)
    {
    case 'k': case 'K':
      value*= 1024;
      break;
    case 'm': case 'M':
      value*= 1024*1024;
      break;
    }
  return value;
}

#if !STACKVM && !COGVM
static int jitArgs(char *str)
{
  char *endptr= str;
  int  args= 3;				/* default JIT mode = fast compiler */
  
  if (*str == '\0') return args;
  if (*str != ',')
    args= strtol(str, &endptr, 10);	/* mode */
  while (*endptr == ',')		/* [,debugFlag]* */
    args|= (1 << (strtol(endptr + 1, &endptr, 10) + 8));
  return args;
}
#endif /* !STACKVM && !COGVM */


# include <locale.h>
static void vm_parseEnvironment(void)
{
  char *ev= setlocale(LC_CTYPE, "");
  if (ev)
    setLocaleEncoding(ev);
  else
    fprintf(stderr, "setlocale() failed (check values of LC_CTYPE, LANG and LC_ALL)\n");

  if (documentName)
    strcpy(shortImageName, documentName);
  else if ((ev= getenv("SQUEAK_IMAGE")))
    strcpy(shortImageName, ev);
  else
    strcpy(shortImageName, "squeak.image");

  if ((ev= getenv("SQUEAK_MEMORY")))	extraMemory= strtobkm(ev);
  if ((ev= getenv("SQUEAK_MMAP")))	useMmap= strtobkm(ev);
  if ((ev= getenv("SQUEAK_PLUGINS")))	squeakPlugins= strdup(ev);
  if ((ev= getenv("SQUEAK_NOEVENTS")))	noEvents= 1;
  if ((ev= getenv("SQUEAK_NOTIMER")))	useItimer= 0;
#if !STACKVM && !COGVM
  if ((ev= getenv("SQUEAK_JIT")))	useJit= jitArgs(ev);
  if ((ev= getenv("SQUEAK_PROCS")))	jitProcs= atoi(ev);
  if ((ev= getenv("SQUEAK_MAXPIC")))	jitMaxPIC= atoi(ev);
#endif /* !STACKVM && !COGVM */
  if ((ev= getenv("SQUEAK_ENCODING")))	setEncoding(&sqTextEncoding, ev);
  if ((ev= getenv("SQUEAK_PATHENC")))	setEncoding(&uxPathEncoding, ev);
  if ((ev= getenv("SQUEAK_TEXTENC")))	setEncoding(&uxTextEncoding, ev);

  if ((ev= getenv("SQUEAK_VM")))	requireModulesNamed(ev);
}


static void usage(void);
static void versionInfo(void);


static int parseModuleArgument(int argc, char **argv, struct SqModule **addr, char *type, char *name)
{
  if (*addr)
    {
      fprintf(stderr, "option '%s' conflicts with previously-loaded module '%s'\n", *argv, (*addr)->name);
      exit(1);
    }
  *addr= requireModule(type, name);
  return (*addr)->parseArgument(argc, argv);
}


static int vm_parseArgument(int argc, char **argv)
{
  /* deal with arguments that implicitly load modules */

  if (!strncmp(argv[0], "-psn_", 5))
    {
      displayModule= requireModule("display", "Quartz");
      return displayModule->parseArgument(argc, argv);
    }

  if ((!strcmp(argv[0], "-vm")) && (argc > 1))
    {
      requireModulesNamed(argv[1]);
      return 2;
    }

  if (!strncmp(argv[0], "-vm-", 4))
    {
      requireModulesNamed(argv[0] + 4);
      return 1;
    }

  /* legacy compatibility */		/*** XXX to be removed at some time ***/

# define moduleArg(arg, type, name)						\
    if (!strcmp(argv[0], arg))							\
      return parseModuleArgument(argc, argv, &type##Module, #type, name);

  moduleArg("-nodisplay",	display, "null");
  moduleArg("-display",		display, "X11");
  moduleArg("-headless",	display, "X11");
  moduleArg("-quartz",		display, "Quartz");
  moduleArg("-nosound",		sound,   "null");

# undef moduleArg

  /* vm arguments */

  if      (!strcmp(argv[0], "-help"))		{ usage();		return 1; }
  else if (!strcmp(argv[0], "-noevents"))	{ noEvents	= 1;	return 1; }
  else if (!strcmp(argv[0], "-nomixer"))	{ noSoundMixer	= 1;	return 1; }
  else if (!strcmp(argv[0], "-notimer"))	{ useItimer	= 0;	return 1; }
  else if (!strcmp(argv[0], "-nohandlers"))	{ installHandlers= 0;	return 1; }
#if !STACKVM && !COGVM
  else if (!strncmp(argv[0],"-jit", 4))		{ useJit	= jitArgs(argv[0]+4);	return 1; }
  else if (!strcmp(argv[0], "-nojit"))		{ useJit	= 0;	return 1; }
  else if (!strcmp(argv[0], "-spy"))		{ withSpy	= 1;	return 1; }
#endif /* !STACKVM && !COGVM */
  else if (!strcmp(argv[0], "-version"))	{ versionInfo();	return 1; }
  else if (!strcmp(argv[0], "-single"))		{ runAsSingleInstance=1; return 1; }
  /* option requires an argument */
  else if (argc > 1)
    {
      if (!strcmp(argv[0], "-memory"))	{ extraMemory=	 strtobkm(argv[1]);	 return 2; }
#if !STACKVM && !COGVM
      else if (!strcmp(argv[0], "-procs"))	{ jitProcs=	 atoi(argv[1]);		 return 2; }
      else if (!strcmp(argv[0], "-maxpic"))	{ jitMaxPIC=	 atoi(argv[1]);		 return 2; }
#endif /* !STACKVM && !COGVM */
      else if (!strcmp(argv[0], "-mmap"))	{ useMmap=	 strtobkm(argv[1]);	 return 2; }
      else if (!strcmp(argv[0], "-plugins"))	{ squeakPlugins= strdup(argv[1]);	 return 2; }
      else if (!strcmp(argv[0], "-encoding"))	{ setEncoding(&sqTextEncoding, argv[1]); return 2; }
      else if (!strcmp(argv[0], "-pathenc"))	{ setEncoding(&uxPathEncoding, argv[1]); return 2; }
#if STACKVM
      else if (!strcmp(argv[0], "-eden")) {
		extern sqInt desiredEdenBytes;
		desiredEdenBytes = strtobkm(argv[1]);
		return 2; }
      else if (!strcmp(argv[0], "-leakcheck")) { 
		extern sqInt checkForLeaks;
		checkForLeaks = atoi(argv[1]);	 
		return 2; }
      else if (!strcmp(argv[0], "-stackpages")) {
		extern sqInt desiredNumStackPages;
		desiredNumStackPages = atoi(argv[1]);
		return 2; }
      else if (!strcmp(argv[0], "-breaksel")) { 
		extern void setBreakSelector(char *);
		setBreakSelector(argv[1]);
		return 2; }
      else if (!strcmp(argv[0], "-noheartbeat")) { 
		extern sqInt suppressHeartbeatFlag;
		suppressHeartbeatFlag = 1;
		return 1; }
      else if (!strcmp(argv[0], "-nocpr")) { 
		extern sqInt defibrilate;
		defibrilate = 0;
		return 1; }
      else if (!strcmp(argv[0], "-pollpip")) { 
		extern sqInt pollpip;
		pollpip = atoi(argv[1]);	 
		return 2; }
#endif /* STACKVM */
#if COGVM
      else if (!strcmp(argv[0], "-codesize")) { 
		extern sqInt desiredCogCodeSize;
		desiredCogCodeSize = strtobkm(argv[1]);	 
		return 2; }
# define TLSLEN (sizeof("-sendtrace")-1)
      else if (!strncmp(argv[0], "-sendtrace", TLSLEN)) { 
		extern int traceLinkedSends;
		char *equalsPos = strchr(argv[0],'=');

		if (!equalsPos) {
			traceLinkedSends = 1;
			return 1;
		}
		if (equalsPos - argv[0] != TLSLEN
		  || (equalsPos[1] != '-' && !isdigit(equalsPos[1])))
			return 0;

		traceLinkedSends = atoi(equalsPos + 1);
		return 1; }
      else if (!strcmp(argv[0], "-tracestores")) { 
		extern sqInt traceStores;
		traceStores = 1;
		return 1; }
      else if (!strcmp(argv[0], "-cogmaxlits")) { 
		extern sqInt maxLiteralCountForCompile;
		maxLiteralCountForCompile = strtobkm(argv[1]);	 
		return 2; }
      else if (!strcmp(argv[0], "-cogminjumps")) { 
		extern sqInt minBackwardJumpCountForCompile;
		minBackwardJumpCountForCompile = strtobkm(argv[1]);	 
		return 2; }
#endif /* COGVM */
      else if (!strcmp(argv[0], "-textenc"))
	{
	  char *buf= (char *)malloc(strlen(argv[1]) + 1);
	  int len, i;
	  strcpy(buf, argv[1]);
	  len= strlen(buf);
	  for (i= 0;  i < len;  ++i)
	    buf[i]= toupper(buf[i]);
	  if ((!strcmp(buf, "UTF8")) || (!strcmp(buf, "UTF-8")))
	    textEncodingUTF8= 1;
	  else
	    setEncoding(&uxTextEncoding, buf);
	  free(buf);
	  return 2;
	}
    }
  return 0;	/* option not recognised */
}


static void vm_printUsage(void)
{
  printf("\nCommon <option>s:\n");
  printf("  -encoding <enc>       set the internal character encoding (default: MacRoman)\n");
  printf("  -help                 print this help message, then exit\n");
  printf("  -memory <size>[mk]    use fixed heap size (added to image size)\n");
  printf("  -mmap <size>[mk]      limit dynamic heap size (default: %dm)\n", DefaultMmapSize);
#if STACKVM
  printf("  -eden <size>[mk]      use given eden size\n");
  printf("  -stackpages <num>     use given number of stack pages\n");
#endif
  printf("  -noevents             disable event-driven input support\n");
  printf("  -nohandlers           disable sigsegv & sigusr1 handlers\n");
  printf("  -pathenc <enc>        set encoding for pathnames (default: UTF-8)\n");
  printf("  -plugins <path>       specify alternative plugin location (see manpage)\n");
  printf("  -textenc <enc>        set encoding for external text (default: UTF-8)\n");
  printf("  -version              print version information, then exit\n");
  printf("  -vm-<sys>-<dev>       use the <dev> driver for <sys> (see below)\n");
#if COGVM
  printf("  -codesize <size>[mk]  set machine code memory to bytes\n");
  printf("  -sendtrace[=num]      enable send tracing (optionally to a specific value)\n");
  printf("  -tracestores          enable store tracing (assert check stores)\n");
  printf("  -cogmaxlits <n>       set max number of literals for methods compiled to machine code\n");
  printf("  -cogminjumps <n>      set min number of backward jumps for interpreted methods to be considered for compilation to machine code\n");
#endif
#if 1
  printf("Deprecated:\n");
# if !(STACKVM || COGVM)
  printf("  -jit                  enable the dynamic compiler (if available)\n");
# endif
  printf("  -notimer              disable interval timer for low-res clock \n");
  printf("  -display <dpy>        quivalent to '-vm-display-X11 -display <dpy>'\n");
  printf("  -headless             quivalent to '-vm-display-X11 -headless'\n");
  printf("  -nodisplay            quivalent to '-vm-display-null'\n");
  printf("  -nomixer              disable modification of mixer settings\n");
  printf("  -nosound              quivalent to '-vm-sound-null'\n");
  printf("  -quartz               quivalent to '-vm-display-Quartz'\n");
#endif
}


static void vm_printUsageNotes(void)
{
  printf("  If `-memory' is not specified then the heap will grow dynamically.\n");
  printf("  <argument>s are ignored, but are processed by the Squeak image.\n");
  printf("  The first <argument> normally names a Squeak `script' to execute.\n");
  printf("  Precede <arguments> by `--' to use default image.\n");
}


static void *vm_makeInterface(void)
{
  fprintf(stderr, "this cannot happen\n");
  abort();
}


SqModuleDefine(vm, Module);


/*** options processing ***/


static void usage(void)
{
  struct SqModule *m= 0;
  printf("Usage: %s [<option>...] [<imageName> [<argument>...]]\n", argVec[0]);
  printf("       %s [<option>...] -- [<argument>...]\n", argVec[0]);
  sqIgnorePluginErrors= 1;
  {
    struct moduleDescription *md;
    for (md= moduleDescriptions;  md->addr;  ++md)
      queryModule(md->type, md->name);
  }
  modulesDo(m)
    m->printUsage();
  if (useJit)
    {
      printf("\njit <option>s:\n");
      printf("  -align <n>            align functions at <n>-byte boundaries\n");
      printf("  -jit<o>[,<d>...]      set optimisation [and debug] levels\n");
      printf("  -maxpic <n>           set maximum PIC size to <n> entries\n");
      printf("  -procs <n>            allow <n> concurrent volatile processes\n");
      printf("  -spy                  enable the system spy\n");
    }
  printf("\nNotes:\n");
  printf("  <imageName> defaults to `squeak.image'.\n");
  modulesDo(m)
    m->printUsageNotes();
  printf("\nAvailable drivers:\n");
  for (m= modules;  m->next;  m= m->next)
    printf("  %s\n", m->name);
  exit(1);
}


char *getVersionInfo(int verbose)
{
  extern int   vm_serial;
  extern char *vm_date, *cc_version, *ux_version;
  char *info= (char *)malloc(4096);
  info[0]= '\0';

  if (verbose)
    sprintf(info+strlen(info), "Squeak VM version: ");
  sprintf(info+strlen(info), "%s #%d", VM_VERSION, vm_serial);
#if defined(USE_XSHM)
  sprintf(info+strlen(info), " XShm");
#endif
  sprintf(info+strlen(info), " %s %s\n", vm_date, cc_version);
  if (verbose)
    sprintf(info+strlen(info), "Built from: ");
  sprintf(info+strlen(info), "%s\n", interpreterVersion);
  if (verbose)
    sprintf(info+strlen(info), "Build host: ");
  sprintf(info+strlen(info), "%s\n", ux_version);
  sprintf(info+strlen(info), "plugin path: %s [default: %s]\n", squeakPlugins, vmPath);
  return info;
}


static void versionInfo(void)
{
  printf("%s", getVersionInfo(0));
  exit(0);
}


static void parseArguments(int argc, char **argv)
{
# define skipArg()	(--argc, argv++)
# define saveArg()	(vmArgVec[vmArgCnt++]= *skipArg())

  saveArg();	/* vm name */

  while ((argc > 0) && (**argv == '-'))	/* more options to parse */
    {
      struct SqModule *m= 0;
      int n= 0;
      if (!strcmp(*argv, "--"))		/* escape from option processing */
	break;
      modulesDo (m)
	if ((n= m->parseArgument(argc, argv)))
	  break;
#    ifdef DEBUG_IMAGE
      printf("parseArgument n = %d\n", n);
#    endif
      if (n == 0)			/* option not recognised */
	{
	  fprintf(stderr, "unknown option: %s\n", argv[0]);
	  usage();
	}
      while (n--)
	saveArg();
    }
  if (!argc)
    return;
  if (!strcmp(*argv, "--"))
    skipArg();
  else					/* image name */
    {
      if (!documentName)
	strcpy(shortImageName, saveArg());
      if (!strstr(shortImageName, ".image"))
	strcat(shortImageName, ".image");
    }
  /* save remaining arguments as Squeak arguments */
  while (argc > 0)
    squeakArgVec[squeakArgCnt++]= *skipArg();

# undef saveArg
# undef skipArg
}


/*** main ***/


static void imageNotFound(char *imageName)
{
  /* image file is not found */
  fprintf(stderr,
	  "Could not open the Squeak image file `%s'.\n"
	  "\n"
	  "There are three ways to open a Squeak image file.  You can:\n"
	  "  1. Put copies of the default image and changes files in this directory.\n"
	  "  2. Put the name of the image file on the command line when you\n"
	  "     run squeak (use the `-help' option for more information).\n"
	  "  3. Set the environment variable SQUEAK_IMAGE to the name of the image\n"
	  "     that you want to use by default.\n"
	  "\n"
	  "For more information, type: `man squeak' (without the quote characters).\n",
	  imageName);
  exit(1);
}


void imgInit(void)
{
  /* read the image file and allocate memory for Squeak heap */
  for (;;)
    {
      FILE *f= 0;
      struct stat sb;
      char imageName[MAXPATHLEN];
      sq2uxPath(shortImageName, strlen(shortImageName), imageName, 1000, 1);
      if ((  (-1 == stat(imageName, &sb)))
	  || ( 0 == (f= fopen(imageName, "r"))))
	{
	  if (dpy->winImageFind(shortImageName, sizeof(shortImageName)))
	    continue;
	  dpy->winImageNotFound();
	  imageNotFound(shortImageName);
	}
      {
	int fd= open(imageName, O_RDONLY);
	if (fd < 0) abort();
#      ifdef DEBUG_IMAGE
	printf("fstat(%d) => %d\n", fd, fstat(fd, &sb));
#      endif
      }
      recordFullPathForImageName(shortImageName); /* full image path */
      if (extraMemory)
	useMmap= 0;
      else
	extraMemory= DefaultHeapSize * 1024 *1024;
#    ifdef DEBUG_IMAGE
      printf("image size %d + heap size %d (useMmap = %d)\n", (int)sb.st_size, extraMemory, useMmap);
#    endif
      extraMemory += (int)sb.st_size;
      readImageFromFileHeapSizeStartingAt(f, extraMemory, 0);
      sqImageFileClose(f);
      break;
    }
}

#if defined(__GNUC__) && ( defined(i386) || defined(__i386) || defined(__i386__)  \
			|| defined(i486) || defined(__i486) || defined (__i486__) \
			|| defined(intel) || defined(x86) || defined(i86pc) )
  static void fldcw(unsigned int cw)
  {
    __asm__("fldcw %0" :: "m"(cw));
  }
#else
# define fldcw(cw)
#endif

#if defined(__GNUC__) && ( defined(ppc) || defined(__ppc) || defined(__ppc__)  \
			|| defined(POWERPC) || defined(__POWERPC) || defined (__POWERPC__) )
  void mtfsfi(unsigned long long fpscr)
  {
    __asm__("lfd   f0, %0" :: "m"(fpscr));
    __asm__("mtfsf 0xff, f0");
  }
#else
# define mtfsfi(fpscr)
#endif

int main(int argc, char **argv, char **envp)
{
  fldcw(0x12bf);	/* signed infinity, round to nearest, REAL8, disable intrs, disable signals */
  mtfsfi(0);		/* disable signals, IEEE mode, round to nearest */

  /* Make parameters global for access from plugins */

  argCnt= argc;
  argVec= argv;
  envVec= envp;

#ifdef DEBUG_IMAGE
  {
    int i= argc;
    char **p= argv;
    while (i--)
      printf("arg: %s\n", *p++);
  }
#endif

  /* Allocate arrays to store copies of pointers to command line
     arguments.  Used by getAttributeIntoLength(). */

  if ((vmArgVec= calloc(argc + 1, sizeof(char *))) == 0)
    outOfMemory();

  if ((squeakArgVec= calloc(argc + 1, sizeof(char *))) == 0)
    outOfMemory();

#if defined(__alpha__) && defined(__osf__)
  /* disable printing of unaligned access exceptions */
  {
    int buf[2]= { SSIN_UACPROC, UAC_NOPRINT };
    if (setsysinfo(SSI_NVPAIRS, buf, 1, 0, 0, 0) < 0)
      {
	perror("setsysinfo(UAC_NOPRINT)");
      }
  }
#endif

#if defined(HAVE_TZSET)
  tzset();	/* should _not_ be necessary! */
#endif

  recordFullPathForVmName(argv[0]); /* full vm path */
  squeakPlugins= vmPath;		/* default plugin location is VM directory */

#if !DEBUG
  sqIgnorePluginErrors= 1;
#endif
  if (!modules)
    modules= &vm_Module;
  vm_Module.parseEnvironment();
  parseArguments(argc, argv);
  if ((!dpy) || (!snd))
    loadModules();
#if !DEBUG
  sqIgnorePluginErrors= 0;
#endif

#if defined(DEBUG_MODULES)
  printf("displayModule %p %s\n", displayModule, displayModule->name);
  if (soundModule)
    printf("soundModule   %p %s\n", soundModule,   soundModule->name);
#endif

  if (!realpath(argv[0], vmName))
    vmName[0]= 0; /* full VM name */

#ifdef DEBUG_IMAGE
  printf("vmName: %s -> %s\n", argv[0], vmName);
  printf("viName: %s\n", shortImageName);
  printf("documentName: %s\n", documentName);
#endif

#if STACKVM || COGVM
  ioInitTime();
  ioInitThreads();
#else
  initTimers();
#endif
  aioInit();
  dpy->winInit();
  imgInit();
  /* If running as a single instance and there are arguments after the image
   * and any are files then try and drop these on the existing instance.
   */
  dpy->winOpen(runAsSingleInstance ? squeakArgCnt : 0, squeakArgVec);

#if defined(HAVE_LIBDL) && !(STACKVM || COGVM)
  if (useJit)
    {
      /* first try to find an internal dynamic compiler... */
      void *handle= ioLoadModule(0);
      void *comp= ioFindExternalFunctionIn("j_interpret", handle);
      /* ...and if that fails... */
      if (comp == 0)
	{
	  /* ...try to find an external one */
	  handle= ioLoadModule("SqueakCompiler");
	  if (handle != 0)
	    comp= ioFindExternalFunctionIn("j_interpret", handle);
	}
      if (comp)
	{
	  ((void (*)(void))comp)();
	  fprintf(stderr, "handing control back to interpret() -- have a nice day\n");
	}
      else
	printf("could not find j_interpret\n");
      exit(1);
    }
#endif /* defined(HAVE_LIBDL) && !(STACKVM || COGVM) */

  if (installHandlers) {
	struct sigaction sigusr1_handler_action, sigsegv_handler_action;

	sigsegv_handler_action.sa_sigaction = sigsegv;
	sigsegv_handler_action.sa_flags = SA_NODEFER | SA_SIGINFO;
	sigemptyset(&sigsegv_handler_action.sa_mask);
    (void)sigaction(SIGSEGV, &sigsegv_handler_action, 0);

	sigusr1_handler_action.sa_sigaction = sigusr1;
	sigusr1_handler_action.sa_flags = SA_NODEFER | SA_SIGINFO;
	sigemptyset(&sigusr1_handler_action.sa_mask);
    (void)sigaction(SIGUSR1, &sigusr1_handler_action, 0);
  }

#if defined(IMAGE_DUMP)
  signal(SIGHUP,  sighup);
  signal(SIGQUIT, sigquit);
#endif

  /* run Squeak */
  if (runInterpreter)
    interpret();

  /* we need these, even if not referenced from main executable */
  (void)sq2uxPath;
  (void)ux2sqPath;
  sqDebugAnchor();
  
  return 0;
}

int ioExit(void) { return ioExitWithErrorCode(0); }

sqInt
ioExitWithErrorCode(int ec)
{
  dpy->winExit();
  exit(ec);
  return ec;
}

#if defined(DARWIN)
# include "mac-alias.c"
#endif


/* Copy aFilenameString to aCharBuffer and optionally resolveAlias (or
   symbolic link) to the real path of the target.  Answer 0 if
   successful of -1 to indicate an error.  Assume aCharBuffer is at
   least PATH_MAX bytes long.  Note that MAXSYMLINKS is a lower bound
   on the (potentially unlimited) number of symlinks allowed in a
   path, but calling sysconf() seems like overkill. */

sqInt sqGetFilenameFromString(char *aCharBuffer, char *aFilenameString, sqInt filenameLength, sqInt resolveAlias)
{
  int numLinks= 0;
  struct stat st;

  memcpy(aCharBuffer, aFilenameString, filenameLength);
  aCharBuffer[filenameLength]= 0;

  if (resolveAlias)
    for (;;)	/* aCharBuffer might refer to link or alias */
      {
	if (!lstat(aCharBuffer, &st) && S_ISLNK(st.st_mode))	/* symlink */
	  { char linkbuf[PATH_MAX+1];
	    if (++numLinks > MAXSYMLINKS)
	      return -1;	/* too many levels of indirection */

	    filenameLength= readlink(aCharBuffer, linkbuf, PATH_MAX);
	    if ((filenameLength < 0) || (filenameLength >= PATH_MAX))
	      return -1;	/* link unavailable or path too long */

	    linkbuf[filenameLength]= 0;

	    if (filenameLength > 0 && *linkbuf == '/') /* absolute */
	      strcpy(aCharBuffer, linkbuf);
	    else {
	      char *lastSeparator = strrchr(aCharBuffer,'/');
	      char *append = lastSeparator ? lastSeparator + 1 : aCharBuffer;
	      if (append - aCharBuffer + strlen(linkbuf) > PATH_MAX)
		return -1; /* path too long */
	      strcpy(append,linkbuf);
	    }

	    continue;
	  }

#    if defined(DARWIN)
	if (isMacAlias(aCharBuffer))
	  {
	    if ((++numLinks > MAXSYMLINKS) || !resolveMacAlias(aCharBuffer, aCharBuffer, PATH_MAX))
	      return -1;		/* too many levels or bad alias */
	    continue;
	  }
#    endif

	break;			/* target is no longer a symlink or alias */
      }

  return 0;
}


sqInt ioGatherEntropy(char *buffer, sqInt bufSize)
{
  int fd, count= 0;

  if ((fd= open("/dev/urandom", O_RDONLY)) < 0)
    return 0;

  while (count < bufSize)
    {
      int n;
      if ((n= read(fd, buffer + count, bufSize)) < 1)
	break;
      count += n;
    }

  close(fd);

  return count == bufSize;
}


#if COGVM
/*
 * Support code for Cog.
 * a) Answer whether the C frame pointer is in use, for capture of the C stack
 *    pointers.
 */
# if defined(i386) || defined(__i386) || defined(__i386__)
/*
 * Cog has already captured CStackPointer  before calling this routine.  Record
 * the original value, capture the pointers again and determine if CFramePointer
 * lies between the two stack pointers and hence is likely in use.  This is
 * necessary since optimizing C compilers for x86 may use %ebp as a general-
 * purpose register, in which case it must not be captured.
 */
int
isCFramePointerInUse()
{
	extern unsigned long CStackPointer, CFramePointer;
	extern void (*ceCaptureCStackPointers)(void);
	unsigned long currentCSP = CStackPointer;

	currentCSP = CStackPointer;
	ceCaptureCStackPointers();
	assert(CStackPointer < currentCSP);
	return CFramePointer >= CStackPointer && CFramePointer <= currentCSP;
}
# endif /* defined(i386) || defined(__i386) || defined(__i386__) */
#endif /* COGVM */
