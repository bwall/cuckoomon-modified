/*
Cuckoo Sandbox - Automated Malware Analysis
Copyright (C) 2010-2014 Cuckoo Sandbox Developers

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stddef.h>
#include "ntapi.h"
#include "capstone/include/capstone.h"
#include "capstone/include/x86.h"
#include "hooking.h"
#include "ignore.h"
#include "unhook.h"
#include "misc.h"
#include "pipe.h"

extern DWORD g_tls_hook_index;

// do not change this number
#define TLS_LAST_ERROR 0x34

void emit_rel(unsigned char *buf, unsigned char *source, unsigned char *target)
{
	*(DWORD *)buf = (DWORD)(target - (source + 4));
}

// need to be very careful about what we call in here, as it can be called in the context of any hook
// including those that hold the loader lock

static int set_caller_info(ULONG_PTR addr)
{
	hook_info_t *hookinfo = hook_info();

	if (!is_in_dll_range(addr)) {
		if (hookinfo->main_caller_retaddr == 0)
			hookinfo->main_caller_retaddr = addr;
		else {
			hookinfo->parent_caller_retaddr = addr;
			return 1;
		}
	}
	return 0;
}

static int addr_in_our_dll_range(ULONG_PTR addr)
{
	if (addr >= g_our_dll_base && addr < (g_our_dll_base + g_our_dll_size))
		return 1;
	return 0;
}

static int operate_on_backtrace(ULONG_PTR retaddr, ULONG_PTR _ebp, int (*func)(ULONG_PTR))
{
	hook_info_t *hookinfo = hook_info();
	int ret;

    ULONG_PTR top = get_stack_top();
    ULONG_PTR bottom = get_stack_bottom();

    unsigned int count = HOOK_BACKTRACE_DEPTH;

	ret = func(retaddr);
	if (ret)
		return ret;

	while (_ebp >= bottom && _ebp <= (top - (2 * sizeof(ULONG_PTR))) && count-- != 0)
	{
        // obtain the return address and the next value of ebp
		ULONG_PTR addr = *(ULONG_PTR *)(_ebp + sizeof(ULONG_PTR));
		_ebp = *(ULONG_PTR *)_ebp;

		ret = func(addr);
		if (ret)
			return ret;
    }

	return ret;
}

int called_by_hook(void)
{
	hook_info_t *hookinfo = hook_info();

	return operate_on_backtrace(hookinfo->return_address, hookinfo->frame_pointer, addr_in_our_dll_range);
}

// returns 1 if we should call our hook, 0 if we should call the original function instead
int WINAPI enter_hook(uint8_t is_special_hook, ULONG_PTR _ebp, ULONG_PTR retaddr)
{
	hook_info_t *hookinfo = hook_info();

	hookinfo->return_address = retaddr;
	hookinfo->frame_pointer = _ebp;

	/* set caller information */
	hookinfo->main_caller_retaddr = 0;
	hookinfo->parent_caller_retaddr = 0;
	operate_on_backtrace(retaddr, _ebp, set_caller_info);

	if ((!called_by_hook() || is_special_hook) && (hookinfo->disable_count < 1))
		return 1;
	return 0;
}

hook_info_t *hook_info()
{
	hook_info_t *ptr;

	DWORD lasterror = our_getlasterror();

	ptr = (hook_info_t *)TlsGetValue(g_tls_hook_index);
	if (ptr == NULL) {
		// this wizardry allows us to hook NtAllocateVirtualMemory -- otherwise we'd crash from infinite
		// recursion if NtAllocateVirtualMemory was the first API we saw on a new thread
		char dummybuf[sizeof(hook_info_t)] = { 0 };

		hook_info_t *info = (hook_info_t *)&dummybuf;
		TlsSetValue(g_tls_hook_index, info);

		// now allocate the memory we need for the hook info struct without calling our hooks
		// shouldn't need to do the disable_count thanks to the new call stack inspection, but
		// it doesn't hurt
		info->disable_count++;
		hook_info_t *newinfo = (hook_info_t *)calloc(1, sizeof(hook_info_t));
		info->disable_count--;

		TlsSetValue(g_tls_hook_index, newinfo);
		ptr = newinfo;
	}

	our_setlasterror(lasterror);

	return ptr;
}

DWORD our_getlasterror(void)
{
	char *teb = (char *)NtCurrentTeb();

	return *(DWORD *)(teb + TLS_LAST_ERROR);
}

// we do our own version of this function to avoid the potential debug triggers
void our_setlasterror(DWORD val)
{
	char *teb = (char *)NtCurrentTeb();

	*(DWORD *)(teb + TLS_LAST_ERROR) = val;
}

void hook_enable()
{
    hook_info()->disable_count--;
}

void hook_disable()
{
    hook_info()->disable_count++;
}
