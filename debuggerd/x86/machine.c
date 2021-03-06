/* system/debuggerd/debuggerd.c
**
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ptrace.h>

#include <corkscrew/ptrace.h>

#include <linux/user.h>

#include "../utility.h"
#include "../machine.h"

#include <cutils/properties.h>
#include <dlfcn.h>

// dump specific process data. This data are defined by external library
typedef void (*dump_ps_data_t)(log_t* log, pid_t tid, uintptr_t addr, bool at_fault);

static void dump_specific_ps_info(log_t* log, pid_t tid, uintptr_t addr, bool at_fault) {
    // Used to get global properties
    char propertyBuffer[PROPERTY_VALUE_MAX];
    memset(propertyBuffer, 0, PROPERTY_VALUE_MAX); // zero out buffer so we don't use junk
    property_get("system.debug.plugins", propertyBuffer, NULL);
    _LOG(log, !at_fault, "\ndump_specific_ps_info: library name: %s\n", propertyBuffer);

    // open the library. Now name used 'as is'
    if (propertyBuffer[0] == 0) {
        return;
    }

    void* handle = dlopen(propertyBuffer, RTLD_LAZY);
    if (handle == NULL) {
        _LOG(log, !at_fault, "\ndump_specific_ps_info: can't open library %s\n", propertyBuffer);
        return; // no library
    }

    // reset errors
    dlerror();
    dump_ps_data_t dump_ps_data = (dump_ps_data_t) dlsym(handle, "dump_ps_data");
    if (dump_ps_data == NULL) {
        const char *dlsym_error = dlerror();
        _LOG(log, !at_fault, "\ndump_specific_ps_info: no required method in library (%s)\n",
                dlsym_error == NULL ? "unknown reason" : dlsym_error);
        dlclose(handle);
        return;
    }

    dump_ps_data(log, tid, addr, at_fault);
    dlclose(handle);
}

void dump_memory_and_code(const ptrace_context_t* context __attribute((unused)),
        log_t* log, pid_t tid, bool at_fault) {
    struct pt_regs_x86 r;

    if (ptrace(PTRACE_GETREGS, tid, 0, &r)) {
        return;
    }
    dump_specific_ps_info(log, tid, (uintptr_t)r.eip, at_fault);
}

void dump_registers(const ptrace_context_t* context __attribute((unused)),
        log_t* log, pid_t tid, bool at_fault) {
    struct pt_regs_x86 r;
    int scopeFlags = (at_fault ? SCOPE_AT_FAULT : 0);

    if(ptrace(PTRACE_GETREGS, tid, 0, &r)) {
        _LOG(log, scopeFlags, "cannot get registers: %s\n", strerror(errno));
        return;
    }
    //if there is no stack, no print just like arm
    if(!r.ebp)
        return;
    _LOG(log, scopeFlags, "    eax %08x  ebx %08x  ecx %08x  edx %08x\n",
         r.eax, r.ebx, r.ecx, r.edx);
    _LOG(log, scopeFlags, "    esi %08x  edi %08x\n",
         r.esi, r.edi);
    _LOG(log, scopeFlags, "    xcs %08x  xds %08x  xes %08x  xfs %08x  xss %08x\n",
         r.xcs, r.xds, r.xes, r.xfs, r.xss);
    _LOG(log, scopeFlags, "    eip %08x  ebp %08x  esp %08x  flags %08x\n",
         r.eip, r.ebp, r.esp, r.eflags);
}
