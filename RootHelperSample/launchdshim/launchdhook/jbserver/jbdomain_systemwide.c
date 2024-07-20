#include "jbserver.h"
// #include "info.h"
#include "sandbox.h"
#include "libproc.h"
#include "libproc_private.h"

// #include <libjailbreak/signatures.h>
// #include <libjailbreak/trustcache.h>
#include "kernel.h"
#include "util.h"
// #include "primitives.h"
#include "codesign.h"
#include <signal.h>
#include "bsm/audit.h"
#include "exec_patch.h"
#include "log.h"
#include "../fun/krw.h"
// extern bool stringStartsWith(const char *str, const char* prefix);
// extern bool stringEndsWith(const char* str, const char* suffix);
bool stringStartsWith(const char *str, const char* prefix)
{
	if (!str || !prefix) {
		return false;
	}

	size_t str_len = strlen(str);
	size_t prefix_len = strlen(prefix);

	if (str_len < prefix_len) {
		return false;
	}

	return !strncmp(str, prefix, prefix_len);
}

bool stringEndsWith(const char* str, const char* suffix)
{
	if (!str || !suffix) {
		return false;
	}

	size_t str_len = strlen(str);
	size_t suffix_len = strlen(suffix);

	if (str_len < suffix_len) {
		return false;
	}

	return !strcmp(str + str_len - suffix_len, suffix);
}

#define APP_PATH_PREFIX "/private/var/containers/Bundle/Application/"

static bool is_app_path(const char* path)
{
    if(!path) return false;

    char rp[PATH_MAX];
    if(!realpath(path, rp)) return false;

    if(strncmp(rp, APP_PATH_PREFIX, sizeof(APP_PATH_PREFIX)-1) != 0)
        return false;

    char* p1 = rp + sizeof(APP_PATH_PREFIX)-1;
    char* p2 = strchr(p1, '/');
    if(!p2) return false;

    //is normal app or jailbroken app/daemon?
    if((p2 - p1) != (sizeof("xxxxxxxx-xxxx-xxxx-yxxx-xxxxxxxxxxxx")-1))
        return false;

	return true;
}

bool is_sub_path(const char* parent, const char* child)
{
	char real_child[PATH_MAX]={0};
	char real_parent[PATH_MAX]={0};

	if(!realpath(child, real_child)) return false;
	if(!realpath(parent, real_parent)) return false;

	if(!stringStartsWith(real_child, real_parent))
		return false;

	return real_child[strlen(real_parent)] == '/';
}

static bool systemwide_domain_allowed(audit_token_t clientToken)
{
	return true;
}

static int systemwide_get_jbroot(char **rootPathOut)
{
	*rootPathOut = strdup(jbinfo(rootPath));
	return 0;
}

static int systemwide_get_boot_uuid(char **bootUUIDOut)
{
	const char *launchdUUID = getenv("LAUNCHD_UUID");
	*bootUUIDOut = launchdUUID ? strdup(launchdUUID) : NULL;
	return 0;
}

char* generate_sandbox_extensions(audit_token_t *processToken, bool writable)
{
	char* sandboxExtensionsOut=NULL;
	char jbrootbase[PATH_MAX];
	char jbrootsecondary[PATH_MAX];
	snprintf(jbrootbase, sizeof(jbrootbase), "/private/var/containers/Bundle/Application/.jbroot-%016llX/", jbinfo(jbrand));
	snprintf(jbrootsecondary, sizeof(jbrootsecondary), "/private/var/mobile/Containers/Shared/AppGroup/.jbroot-%016llX/", jbinfo(jbrand));

	char* fileclass = writable ? "com.apple.app-sandbox.read-write" : "com.apple.app-sandbox.read";

	char *readExtension = sandbox_extension_issue_file_to_process("com.apple.app-sandbox.read", jbrootbase, 0, *processToken);
	char *execExtension = sandbox_extension_issue_file_to_process("com.apple.sandbox.executable", jbrootbase, 0, *processToken);
	char *readExtension2 = sandbox_extension_issue_file_to_process(fileclass, jbrootsecondary, 0, *processToken);
	if (readExtension && execExtension && readExtension2) {
		char extensionBuf[strlen(readExtension) + 1 + strlen(execExtension) + strlen(readExtension2) + 1];
		strcat(extensionBuf, readExtension);
		strcat(extensionBuf, "|");
		strcat(extensionBuf, execExtension);
		strcat(extensionBuf, "|");
		strcat(extensionBuf, readExtension2);
		sandboxExtensionsOut = strdup(extensionBuf);
	}
	if (readExtension) free(readExtension);
	if (execExtension) free(execExtension);
	if (readExtension2) free(readExtension2);
	return sandboxExtensionsOut;
}

static int systemwide_process_checkin(audit_token_t *processToken, char **rootPathOut, char **bootUUIDOut, char **sandboxExtensionsOut, bool *fullyDebuggedOut)
{
	// Fetch process info
	pid_t pid = audit_token_to_pid(*processToken);
	char procPath[4*MAXPATHLEN];
	if (proc_pidpath(pid, procPath, sizeof(procPath)) <= 0) {
		return -1;
	}

	// Find proc in kernelspace
	uint64_t proc = proc_find(pid);
	if (!proc) {
		return -1;
	}

	// Get jbroot and boot uuid
	systemwide_get_jbroot(rootPathOut);
	systemwide_get_boot_uuid(bootUUIDOut);


	struct statfs fs;
	bool isPlatformProcess = statfs(procPath, &fs)==0 && strcmp(fs.f_mntonname, "/private/var") != 0;

	// Generate sandbox extensions for the requesting process
	*sandboxExtensionsOut = generate_sandbox_extensions(processToken, isPlatformProcess);

	bool fullyDebugged = false;
	if (is_app_path(procPath) || is_sub_path(JBRootPath("/Applications"), procPath)) {
		// This is an app, enable CS_DEBUGGED based on user preference
		if (jbsetting(markAppsAsDebugged)) {
			fullyDebugged = true;
		}
	}
	*fullyDebuggedOut = fullyDebugged;
	
	// Allow invalid pages
	cs_allow_invalid(proc, fullyDebugged);

	// Fix setuid
	struct stat sb;
	if (stat(procPath, &sb) == 0) {
		if (S_ISREG(sb.st_mode) && (sb.st_mode & (S_ISUID | S_ISGID))) {
			uint64_t ucred = proc_ucred(proc);
			if ((sb.st_mode & (S_ISUID))) {
				kwrite32(proc + koffsetof(proc, svuid), sb.st_uid);
				kwrite32(ucred + koffsetof(ucred, svuid), sb.st_uid);
				kwrite32(ucred + koffsetof(ucred, uid), sb.st_uid);
			}
			if ((sb.st_mode & (S_ISGID))) {
				kwrite32(proc + koffsetof(proc, svgid), sb.st_gid);
				kwrite32(ucred + koffsetof(ucred, svgid), sb.st_gid);
				kwrite32(ucred + koffsetof(ucred, groups), sb.st_gid);
			}
			uint32_t flag = kread32(proc + koffsetof(proc, flag));
			if ((flag & P_SUGID) != 0) {
				flag &= ~P_SUGID;
				kwrite32(proc + koffsetof(proc, flag), flag);
			}
		}
	}

	// In iOS 16+ there is a super annoying security feature called Protobox
	// Amongst other things, it allows for a process to have a syscall mask
	// If a process calls a syscall it's not allowed to call, it immediately crashes
	// Because for tweaks and hooking this is unacceptable, we update these masks to be 1 for all syscalls on all processes
	// That will at least get rid of the syscall mask part of Protobox
	if (__builtin_available(iOS 16.0, *)) {
		proc_allow_all_syscalls(proc);
	}

	// For whatever reason after SpringBoard has restarted, AutoFill and other stuff stops working
	// The fix is to always also restart the kbd daemon alongside SpringBoard
	// Seems to be something sandbox related where kbd doesn't have the right extensions until restarted
	if (strcmp(procPath, "/System/Library/CoreServices/SpringBoard.app/SpringBoard") == 0) {
		static bool springboardStartedBefore = false;
		if (!springboardStartedBefore) {
			// Ignore the first SpringBoard launch after userspace reboot
			// This fix only matters when SpringBoard gets restarted during runtime
			springboardStartedBefore = true;
		}
		else {
			dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
				killall("/System/Library/TextInput/kbd", false);
			});
		}
	}
	// For the Dopamine app itself we want to give it a saved uid/gid of 0, unsandbox it and give it CS_PLATFORM_BINARY
	// This is so that the buttons inside it can work when jailbroken, even if the app was not installed by TrollStore
	// else if (stringEndsWith(procPath, "/Dopamine.app/Dopamine")) {
	// 	char roothidefile[PATH_MAX];
	// 	snprintf(roothidefile, sizeof(roothidefile), "%s.roothide",procPath);
	// 	if(access(roothidefile, F_OK)==0) {
	// 		// svuid = 0, svgid = 0
	// 		uint64_t ucred = proc_ucred(proc);
	// 		kwrite32(proc + koffsetof(proc, svuid), 0);
	// 		kwrite32(ucred + koffsetof(ucred, svuid), 0);
	// 		kwrite32(proc + koffsetof(proc, svgid), 0);
	// 		kwrite32(ucred + koffsetof(ucred, svgid), 0);

	// 		// platformize
	// 		proc_csflags_set(proc, CS_PLATFORM_BINARY);
	// 	} else {
	// 		kill(pid, SIGKILL);
	// 	}
	// }

	proc_rele(proc);
	return 0;
}

static int systemwide_patch_exec_add(audit_token_t *callerToken, const char* exec_path, bool resume)
{
    uint64_t callerPid = audit_token_to_pid(*callerToken);
    if (callerPid > 0) {
        patchExecAdd((int)callerPid, exec_path, resume);
        return 0;
    }
    return -1;
}

static int systemwide_patch_exec_del(audit_token_t *callerToken, const char* exec_path)
{
    uint64_t callerPid = audit_token_to_pid(*callerToken);
    if (callerPid > 0){
        patchExecDel((int)callerPid, exec_path);
        return 0;
    }
    return -1;
}

struct jbserver_domain gSystemwideDomain = {
	.permissionHandler = systemwide_domain_allowed,
	.actions = {
		// JBS_SYSTEMWIDE_GET_JBROOT
		{
			.handler = systemwide_get_jbroot,
			.args = (jbserver_arg[]){
				{ .name = "root-path", .type = JBS_TYPE_STRING, .out = true },
				{ 0 },
			},
		},
		// JBS_SYSTEMWIDE_GET_BOOT_UUID
		{
			.handler = systemwide_get_boot_uuid,
			.args = (jbserver_arg[]){
				{ .name = "boot-uuid", .type = JBS_TYPE_STRING, .out = true },
				{ 0 },
			},
		},
		// JBS_SYSTEMWIDE_PROCESS_CHECKIN
		{
			.handler = systemwide_process_checkin,
			.args = (jbserver_arg[]) {
				{ .name = "caller-token", .type = JBS_TYPE_CALLER_TOKEN, .out = false },
				{ .name = "root-path", .type = JBS_TYPE_STRING, .out = true },
				{ .name = "boot-uuid", .type = JBS_TYPE_STRING, .out = true },
				{ .name = "sandbox-extensions", .type = JBS_TYPE_STRING, .out = true },
				// { .name = "fully-debugged", .type = JBS_TYPE_BOOL, .out = true },
				{ 0 },
			},
		},
		// // JBS_SYSTEMWIDE_FORK_FIX
		// {
		// 	.handler = systemwide_fork_fix,
		// 	.args = (jbserver_arg[]) {
		// 		{ .name = "caller-token", .type = JBS_TYPE_CALLER_TOKEN, .out = false },
		// 		{ .name = "child-pid", .type = JBS_TYPE_UINT64, .out = false },
		// 		{ 0 },
		// 	},
		// },
		// // JBS_SYSTEMWIDE_CS_REVALIDATE
		// {
		// 	.handler = systemwide_cs_revalidate,
		// 	.args = (jbserver_arg[]) {
		// 		{ .name = "caller-token", .type = JBS_TYPE_CALLER_TOKEN, .out = false },
		// 		{ 0 },
		// 	},
		// },
        // // JBS_SYSTEMWIDE_CS_DROP_GET_TASK_ALLOW
        // {
        //     // .action = JBS_SYSTEMWIDE_CS_DROP_GET_TASK_ALLOW,
        //     .handler = systemwide_cs_drop_get_task_allow,
        //     .args = (jbserver_arg[]) {
        //             { .name = "caller-token", .type = JBS_TYPE_CALLER_TOKEN, .out = false },
        //             { 0 },
        //     },
        // },
        // // JBS_SYSTEMWIDE_PATCH_SPAWN
        // {
        //     // .action = JBS_SYSTEMWIDE_PATCH_SPAWN,
        //     .handler = systemwide_patch_spawn,
        //     .args = (jbserver_arg[]) {
        //             { .name = "caller-token", .type = JBS_TYPE_CALLER_TOKEN, .out = false },
        //             { .name = "pid", .type = JBS_TYPE_UINT64, .out = false },
        //             { .name = "resume", .type = JBS_TYPE_BOOL, .out = false },
        //             { 0 },
        //     },
        // },
        // // JBS_SYSTEMWIDE_PATCH_EXEC_ADD
        // {
        //     // .action = JBS_SYSTEMWIDE_PATCH_EXEC_ADD,
        //     .handler = systemwide_patch_exec_add,
        //     .args = (jbserver_arg[]) {
        //             { .name = "caller-token", .type = JBS_TYPE_CALLER_TOKEN, .out = false },
        //             { .name = "exec-path", .type = JBS_TYPE_STRING, .out = false },
        //             { .name = "resume", .type = JBS_TYPE_BOOL, .out = false },
        //             { 0 },
        //     },
        // },
        // // JBS_SYSTEMWIDE_PATCH_EXEC_DEL
        // {
        //     // .action = JBS_SYSTEMWIDE_PATCH_EXEC_DEL,
        //     .handler = systemwide_patch_exec_del,
        //     .args = (jbserver_arg[]) {
        //             { .name = "caller-token", .type = JBS_TYPE_CALLER_TOKEN, .out = false },
        //             { .name = "exec-path", .type = JBS_TYPE_STRING, .out = false },
        //             { 0 },
        //     },
        // },
		{ 0 },
	},
};