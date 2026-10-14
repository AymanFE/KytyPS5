#include "common/guestProbe.h"

#include "common/logging/log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace Common::GuestProbe {

#if defined(_WIN32)

namespace {

struct MemSpec {
	int      reg    = 0;
	uint64_t off    = 0;
	bool     deref  = false;
	uint64_t off2   = 0;
	uint64_t length = 0x40;
	bool     condition = false; // '!' prefix: log only when these bytes are not all zero
	bool     write     = false; // '=' prefix: store the 8-byte value <len> at the address (experiments)
};

struct Probe {
	uint64_t             va       = 0;
	uint64_t             address  = 0;
	uint32_t             max_hits = 8;
	uint32_t             hits     = 0;
	uint64_t             passes   = 0;
	uint8_t              original = 0;
	bool                 armed    = false;
	bool                 deferred = false; // '^' prefix: armed by ArmDeferred()
	std::vector<MemSpec> mem;
};

constexpr const char* kRegNames[16] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                                       "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

std::mutex         g_mutex;
std::vector<Probe> g_probes;
uint64_t           g_base = 0;
thread_local Probe* t_rearm = nullptr;

uint64_t RegValue(const CONTEXT& c, int reg) {
	switch (reg) {
		case 0: return c.Rax;
		case 1: return c.Rbx;
		case 2: return c.Rcx;
		case 3: return c.Rdx;
		case 4: return c.Rsi;
		case 5: return c.Rdi;
		case 6: return c.Rbp;
		case 7: return c.Rsp;
		case 8: return c.R8;
		case 9: return c.R9;
		case 10: return c.R10;
		case 11: return c.R11;
		case 12: return c.R12;
		case 13: return c.R13;
		case 14: return c.R14;
		default: return c.R15;
	}
}

bool Readable(uint64_t address, uint64_t size) {
	if (address < 0x10000 || size == 0) {
		return false;
	}
	uint64_t page = address & ~uint64_t {0xfff};
	while (page < address + size) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(page), &info, sizeof(info)) == 0 ||
		    info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
		    (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
		                     PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) == 0) {
			return false;
		}
		page += 0x1000;
	}
	return true;
}

void WriteByte(uint64_t address, uint8_t value) {
	DWORD old = 0;
	VirtualProtect(reinterpret_cast<void*>(address), 1, PAGE_EXECUTE_READWRITE, &old);
	*reinterpret_cast<volatile uint8_t*>(address) = value;
	DWORD unused = 0;
	VirtualProtect(reinterpret_cast<void*>(address), 1, old, &unused);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), 1);
}

std::string Describe(const Probe& probe, const CONTEXT& c) {
	std::string line = fmt::format("GuestProbe: 0x{:x} hit {} tid {}:", probe.va, probe.hits,
	                               GetCurrentThreadId());
	for (int r = 0; r < 16; r++) {
		line += fmt::format(" {}={:x}", kRegNames[r], RegValue(c, r));
	}
	if (Readable(c.Rsp, 8)) {
		const uint64_t ret = *reinterpret_cast<const uint64_t*>(c.Rsp);
		line += fmt::format(" ret={:x}", ret >= g_base ? ret - g_base : ret);
	}
	for (const auto& m: probe.mem) {
		if (m.write) {
			line += fmt::format(" write({}+{:x})={:x}", kRegNames[m.reg], m.off, m.length);
			continue;
		}
		uint64_t address = RegValue(c, m.reg) + m.off;
		std::string what  = fmt::format("{}+{:x}", kRegNames[m.reg], m.off);
		if (m.deref) {
			if (!Readable(address, 8)) {
				line += fmt::format(" [{}]=unreadable", what);
				continue;
			}
			address = *reinterpret_cast<const uint64_t*>(address) + m.off2;
			what += fmt::format("*{:x}", m.off2);
		}
		line += fmt::format(" mem({}@{:x})=", what, address);
		if (!Readable(address, m.length)) {
			line += "unreadable";
			continue;
		}
		const auto* bytes = reinterpret_cast<const uint8_t*>(address);
		for (uint64_t i = 0; i < m.length; i++) {
			line += fmt::format("{:02x}", bytes[i]);
			if ((i & 7u) == 7u && i + 1 < m.length) {
				line += '.';
			}
		}
	}
	line += '\n';
	return line;
}

std::atomic<uint64_t> g_watch_address {0};
std::atomic<uint32_t> g_watch_logged {0};

LONG WINAPI Handler(PEXCEPTION_POINTERS e) {
	const auto code = e->ExceptionRecord->ExceptionCode;
	auto&      c    = *e->ContextRecord;
	if (code == EXCEPTION_SINGLE_STEP && (c.Dr6 & 1u) != 0 && g_watch_address.load() != 0) {
		const auto watch = g_watch_address.load();
		c.Dr6 &= ~uint64_t {0xf};
		if (g_watch_logged.fetch_add(1) < 64) {
			std::string line = fmt::format("GuestWatch: write to 0x{:x} = {:x} by pc {:x} (host {:x}) tid {}:",
			                               watch, Readable(watch, 8) ? *reinterpret_cast<const uint64_t*>(watch) : 0,
			                               c.Rip >= g_base ? c.Rip - g_base : c.Rip, c.Rip,
			                               GetCurrentThreadId());
			for (int r = 0; r < 16; r++) {
				line += fmt::format(" {}={:x}", kRegNames[r], RegValue(c, r));
			}
			if (Readable(c.Rsp, 8)) {
				const uint64_t ret = *reinterpret_cast<const uint64_t*>(c.Rsp);
				line += fmt::format(" [rsp]={:x}", ret >= g_base ? ret - g_base : ret);
			}
			if (Readable(c.Rbp + 8, 8)) {
				const uint64_t ret = *reinterpret_cast<const uint64_t*>(c.Rbp + 8);
				line += fmt::format(" [rbp+8]={:x}", ret >= g_base ? ret - g_base : ret);
			}
			line += '\n';
			Log::WriteToConsoleAndLog(line);
		}
		if (t_rearm == nullptr) {
			return EXCEPTION_CONTINUE_EXECUTION;
		}
	}
	if (code == EXCEPTION_SINGLE_STEP) {
		Probe* probe = t_rearm;
		if (probe == nullptr) {
			return EXCEPTION_CONTINUE_SEARCH;
		}
		t_rearm = nullptr;
		std::scoped_lock lock(g_mutex);
		if (probe->hits < probe->max_hits) {
			WriteByte(probe->address, 0xcc);
		} else {
			probe->armed = false;
		}
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	if (code != EXCEPTION_BREAKPOINT) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	const auto address = reinterpret_cast<uint64_t>(e->ExceptionRecord->ExceptionAddress);
	std::string line;
	{
		std::scoped_lock lock(g_mutex);
		Probe* probe = nullptr;
		for (auto& p: g_probes) {
			if (p.armed && p.address == address) {
				probe = &p;
			}
		}
		if (probe == nullptr) {
			return EXCEPTION_CONTINUE_SEARCH;
		}
		probe->passes++;
		bool matched = true;
		for (const auto& m: probe->mem) {
			if (!m.condition) {
				continue;
			}
			uint64_t at = RegValue(c, m.reg) + m.off;
			if (m.deref) {
				at = Readable(at, 8) ? *reinterpret_cast<const uint64_t*>(at) + m.off2 : 0;
			}
			bool nonzero = false;
			if (at != 0 && Readable(at, m.length)) {
				for (uint64_t i = 0; i < m.length; i++) {
					nonzero = nonzero || reinterpret_cast<const uint8_t*>(at)[i] != 0;
				}
			}
			matched = matched && nonzero;
		}
		if (matched) {
			probe->hits++;
			if (probe->hits <= probe->max_hits) {
				line = Describe(*probe, c);
			}
			for (const auto& m: probe->mem) {
				if (!m.write) {
					continue;
				}
				uint64_t at = RegValue(c, m.reg) + m.off;
				if (m.deref) {
					at = Readable(at, 8) ? *reinterpret_cast<const uint64_t*>(at) + m.off2 : 0;
				}
				if (at != 0 && Readable(at, 8)) {
					*reinterpret_cast<uint64_t*>(at) = m.length;
				}
			}
		}
		if ((probe->passes % 1000u) == 0u) {
			line += fmt::format("GuestProbe: 0x{:x} passes {}, logged {}\n", probe->va,
			                    probe->passes, probe->hits);
		}
		WriteByte(address, probe->original);
		c.Rip = address;
		c.EFlags |= 0x100u;
		t_rearm = probe;
	}
	if (!line.empty()) {
		Log::WriteToConsoleAndLog(line);
	}
	return EXCEPTION_CONTINUE_EXECUTION;
}

int ParseReg(std::string_view name) {
	for (int r = 0; r < 16; r++) {
		if (name == kRegNames[r]) {
			return r;
		}
	}
	return -1;
}

std::vector<std::string_view> Split(std::string_view text, char sep) {
	std::vector<std::string_view> parts;
	size_t                        start = 0;
	while (start <= text.size()) {
		const auto end = text.find(sep, start);
		const auto part = text.substr(start, end == std::string_view::npos ? text.npos : end - start);
		if (!part.empty()) {
			parts.push_back(part);
		}
		if (end == std::string_view::npos) {
			break;
		}
		start = end + 1;
	}
	return parts;
}

uint64_t Hex(std::string_view text) {
	return std::strtoull(std::string(text).c_str(), nullptr, 16);
}

} // namespace

void Arm(uint64_t base, uint64_t size) {
	if (g_base != 0) {
		return;
	}
	g_base          = base;
	const char* env = std::getenv("KYTY_GUEST_PROBE");
	if (env == nullptr || *env == '\0') {
		return;
	}
	std::scoped_lock lock(g_mutex);
	std::string text = env;
	if (text[0] == '@') {
		// @<file>: one probe per line (lines starting with '#' are comments).
		text.clear();
		if (std::FILE* file = std::fopen(env + 1, "rb"); file != nullptr) {
			bool comment = false;
			bool start   = true;
			for (int ch = std::fgetc(file); ch != EOF; ch = std::fgetc(file)) {
				if (ch == '\r' || ch == '\n') {
					text += ';';
					comment = false;
					start   = true;
					continue;
				}
				if (start && ch == '#') {
					comment = true;
				}
				start = false;
				if (!comment && ch != ' ' && ch != '\t') {
					text += static_cast<char>(ch);
				}
			}
			std::fclose(file);
		} else {
			Log::WriteToConsoleAndLog(fmt::format("GuestProbe: cannot open '{}'\n", env + 1));
		}
	}
	for (auto spec: Split(text, ';')) {
		const auto fields = Split(spec, ',');
		if (fields.empty()) {
			continue;
		}
		Probe probe;
		auto  head = fields[0];
		if (!head.empty() && head[0] == '^') {
			probe.deferred = true;
			head           = head.substr(1);
		}
		if (const auto at = head.find('@'); at != head.npos) {
			probe.max_hits = static_cast<uint32_t>(std::strtoul(std::string(head.substr(at + 1)).c_str(), nullptr, 10));
			head           = head.substr(0, at);
		}
		probe.va = Hex(head);
		for (size_t i = 1; i < fields.size(); i++) {
			auto    f = fields[i];
			MemSpec m;
			if (!f.empty() && f[0] == '!') {
				m.condition = true;
				f           = f.substr(1);
			}
			if (!f.empty() && f[0] == '=') {
				m.write = true;
				f       = f.substr(1);
			}
			if (const auto colon = f.find(':'); colon != f.npos) {
				m.length = Hex(f.substr(colon + 1));
				f        = f.substr(0, colon);
			}
			if (const auto star = f.find('*'); star != f.npos) {
				m.deref = true;
				m.off2  = Hex(f.substr(star + 1));
				f       = f.substr(0, star);
			}
			const auto sign = f.find_first_of("+-");
			m.reg           = ParseReg(f.substr(0, sign));
			m.off           = sign == f.npos ? 0 : Hex(f.substr(sign + 1));
			if (sign != f.npos && f[sign] == '-') {
				m.off = uint64_t {0} - m.off;
			}
			if (m.reg < 0 || (!m.write && (m.length == 0 || m.length > 0x1000))) {
				Log::WriteToConsoleAndLog(fmt::format("GuestProbe: bad memory spec '{}'\n", fields[i]));
				continue;
			}
			probe.mem.push_back(m);
		}
		if (probe.va >= size || probe.max_hits == 0) {
			Log::WriteToConsoleAndLog(fmt::format("GuestProbe: bad probe '{}'\n", spec));
			continue;
		}
		probe.address = base + probe.va;
		g_probes.push_back(probe);
	}
	if (g_probes.empty()) {
		return;
	}
	AddVectoredExceptionHandler(1, Handler);
	for (auto& p: g_probes) {
		p.original = *reinterpret_cast<const uint8_t*>(p.address);
		if (p.deferred) {
			Log::WriteToConsoleAndLog(fmt::format(
			    "GuestProbe: deferred 0x{:x} ({} hits, {} dumps; armed by ArmDeferred)\n", p.va,
			    p.max_hits, p.mem.size()));
			continue;
		}
		WriteByte(p.address, 0xcc);
		p.armed = true;
		Log::WriteToConsoleAndLog(fmt::format(
		    "GuestProbe: armed 0x{:x} (host 0x{:x}, byte {:02x}, {} hits, {} dumps)\n", p.va,
		    p.address, p.original, p.max_hits, p.mem.size()));
	}
}

void ArmDeferred(const char* reason) {
	std::scoped_lock lock(g_mutex);
	for (auto& p: g_probes) {
		if (!p.deferred || p.armed || p.hits != 0) {
			continue;
		}
		WriteByte(p.address, 0xcc);
		p.armed = true;
		Log::WriteToConsoleAndLog(fmt::format("GuestProbe: armed deferred 0x{:x} ({})\n", p.va,
		                                      reason != nullptr ? reason : ""));
	}
}

void ArmWriteWatch(uint64_t address) {
	if (address == 0 || g_watch_address.exchange(address) != 0) {
		return;
	}
	static std::once_flag handler_once;
	std::call_once(handler_once, [] { AddVectoredExceptionHandler(1, Handler); });
	const DWORD self  = GetCurrentThreadId();
	uint32_t    armed = 0;
	HANDLE      snap  = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap != INVALID_HANDLE_VALUE) {
		THREADENTRY32 entry {};
		entry.dwSize = sizeof(entry);
		for (BOOL ok = Thread32First(snap, &entry); ok; ok = Thread32Next(snap, &entry)) {
			if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self) {
				continue;
			}
			HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
			                           FALSE, entry.th32ThreadID);
			if (thread == nullptr) {
				continue;
			}
			if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
				CONTEXT ctx {};
				ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
				if (GetThreadContext(thread, &ctx) != 0) {
					ctx.Dr0 = address;
					// L0, RW0 = 01 (write), LEN0 = 10 (8 bytes)
					ctx.Dr7 = (ctx.Dr7 & ~uint64_t {0xf0003}) | 1u | (1u << 16u) | (2u << 18u);
					if (SetThreadContext(thread, &ctx) != 0) {
						armed++;
					}
				}
				ResumeThread(thread);
			}
			CloseHandle(thread);
		}
		CloseHandle(snap);
	}
	Log::WriteToConsoleAndLog(fmt::format("GuestWatch: write watch on 0x{:x} armed on {} threads\n", address, armed));
}

#else

void Arm(uint64_t /*base*/, uint64_t /*size*/) {}
void ArmDeferred(const char* /*reason*/) {}
void ArmWriteWatch(uint64_t /*address*/) {}

#endif

} // namespace Common::GuestProbe
