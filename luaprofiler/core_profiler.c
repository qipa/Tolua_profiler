﻿/*
** LuaProfiler
** Copyright Kepler Project 2005.2007 (http://www.keplerproject.org/luaprofiler)
** $Id: core_profiler.c,v 1.10 2009-01-29 12:39:28 jasonsantos Exp $
*/

/*****************************************************************************
core_profiler.c:
   Lua version independent profiler interface.
   Responsible for handling the "enter function" and "leave function" events
   and for writing the log file.

Design (using the Lua callhook mechanism) :
   'lprofP_init_core_profiler' set up the profile service
   'lprofP_callhookIN'         called whenever Lua enters a function
   'lprofP_callhookOUT'        called whenever Lua leaves a function
*****************************************************************************/

/*****************************************************************************
   The profiled program can be viewed as a graph with the following properties:
directed, multigraph, cyclic and connected. The log file generated by a
profiler section corresponds to a path on this graph.
   There are several graphs for which this path fits on. Some times it is
easier to consider this path as being generated by a simpler graph without
properties like cyclic and multigraph.
   The profiler log file can be viewed as a "reversed" depth-first search
(with the depth-first search number for each vertex) vertex listing of a graph
with the following properties: simple, acyclic, directed and connected, for
which each vertex appears as many times as needed to strip the cycles and
each vertex has an indegree of 1.
   "reversed" depth-first search means that instead of being "printed" before
visiting the vertex's descendents (as done in a normal depth-first search),
the vertex is "printed" only after all his descendents have been processed (in
a depth-first search recursive algorithm).
*****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "function_meter.h"

#include "core_profiler.h"
#include "stack.h"

    /* default log name (%s is used to place a random string) */
#define OUT_FILENAME "lprof_%s.out"

int nPrevStackLevel = 0;

    /* for faster execution (??) */
//static FILE *outf;
static lprofS_STACK_RECORD *info;
static float function_call_time;


/* do not allow a string with '\n' and '|' (log file format reserved chars) */
/* - replace them by ' '                                                    */
static void formats(char *s) {
  int i;
  if (!s)
    return;
  for (i = (int)strlen(s); i>=0; i--) {
    if ((s[i] == '|') || (s[i] == '\n'))
      s[i] = ' ';
  }
}

/*
	将lua api的操作记录剔除
	2016-08-10 lennon.c
*/

char modFunFilter[50][2][128];
int modFunFilterNum = 0;

int filter_lua_api(char* func_name, char* mod_name)
{
	/*static char *lua_api[] = {
		"assert", "unpack", "__index", "__newindex", "setmetatable", "getmetatable", "rawget", "type",
		"remove", NULL };*/

	/*static char *lua_api[] = {
		"Tick" };*/

	static char *sharp = "*";
	
	int i = 0;
	while (i < modFunFilterNum)
	{
		if ((strcmp(modFunFilter[i][0], sharp) == 0 || strcmp(modFunFilter[i][0], func_name) == 0) && (strcmp(modFunFilter[i][1], sharp) == 0 || !mod_name || strcmp(modFunFilter[i][1], mod_name) == 0))
		{
			return 1;
		}
		i++;
	}

	return 0;
}

/* computes new stack and new timer */
void lprofP_callhookIN(lprofP_STATE* S, char *func_name, char *file, int linedefined, int currentline,char* what, char* cFun, lprof_DebugInfo* dbg_info) 
{
	if (!func_name || !filter_lua_api(func_name, dbg_info->p_source))
		return;

	if (S->stack_top && dbg_info->level <= S->stack_top->level)
		return;

	S->stack_level++;

	//debugLog("in  m:%s f:%s level:%d curToplevel:%d stack:%d\n", dbg_info->p_source, dbg_info->p_name, dbg_info->level, S->stack_top ? S->stack_top->level : -1, S->stack_level);

	lprofM_enter_function(S, file, func_name, linedefined, currentline, what, cFun, dbg_info);
  
}


/* pauses all timers to write a log line and computes the new stack */
/* returns if there is another function in the stack */
int lprofP_callhookOUT(lprofP_STATE* S, lprof_DebugInfo* dbg_info) {
	// 过滤lua api操作 2016-08-10 lennon.c
	
	if (!dbg_info->p_name || !filter_lua_api(dbg_info->p_name, dbg_info->p_source))
		return 0;

	if (!S->stack_top)
		return 0;

	if (S->stack_level == 0) {
		return 0;
	}

	while (dbg_info->level < S->stack_top->level)
	{
		lprofM_pop_invalid_function(S);
		S->stack_level--;
	}
	
	S->stack_level--;

	//debugLog("out  m:%s f:%s level:%d curToplevel:%d stack:%d\n", dbg_info->p_source, dbg_info->p_name, dbg_info->level, S->stack_top->level, S->stack_level);

	/* 0: do not resume the parent function's timer yet... */
	info = lprofM_leave_function(S, 0, dbg_info);

	/*if (S->stack_level == 0)
		lprofT_print();*/
	if (S->stack_level == 0)
	{
		//debugLog("lprofT_tojson!!\n");
		lprofT_tojson();
	}
		
	/* ... now it's ok to resume the timer */
	if (S->stack_level != 0) {
		lprofM_resume_function(S);
	}

	return 1;
}


/* opens the log file */
/* returns true if the file could be opened */
lprofP_STATE* lprofP_init_core_profiler(const char *_out_filename, int isto_printheader, float _function_call_time) {
  lprofP_STATE* S;
  char auxs[256];
  char *s;
  char *randstr;
  const char *out_filename;

  function_call_time = _function_call_time;
  out_filename = (_out_filename) ? (_out_filename):(OUT_FILENAME);
        
  /* the random string to build the logname is extracted */
  /* from 'tmpnam()' (the '/tmp/' part is deleted)     */
  randstr = tmpnam(NULL);
  for (s = strtok(randstr, "/\\"); s; s = strtok(NULL, "/\\")) {
    randstr = s;
  }

  if(randstr[strlen(randstr)-1]=='.')
    randstr[strlen(randstr)-1]='\0';

  sprintf(auxs, out_filename, randstr);
  outf = fopen(auxs, "w");
  if (!outf) {
    return 0;
  }

  /* initialize the 'function_meter' */
  S = lprofM_init();
  if(!S) {
    fclose(outf);
    return 0;
  }
  lprofP_open();  
  return S;
}

void lprofP_close_core_profiler(lprofP_STATE* S) {  
  lprofT_close();
  if (outf)
	fclose(outf);
  if(S) 
	free(S);
}

lprofP_STATE* lprofP_create_profiler(float _function_call_time) {
  lprofP_STATE* S;

  function_call_time = _function_call_time;

  /* initialize the 'function_meter' */
  S = lprofM_init();
  if(!S) {
    return 0;
  }
    
  return S;
}

