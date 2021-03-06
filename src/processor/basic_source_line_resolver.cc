// Copyright (c) 2010 Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// basic_source_line_resolver.cc: BasicSourceLineResolver implementation.
//
// See basic_source_line_resolver.h and basic_source_line_resolver_types.h
// for documentation.

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <limits>
#include <map>
#include <utility>
#include <vector>
#include <sstream>

#include "common/dwarf/dwarf2enums.h"
#include "processor/basic_source_line_resolver_types.h"
#include "google_breakpad/processor/basic_source_line_resolver.h"
#include "processor/module_factory.h"

#include "processor/tokenize.h"

using std::map;
using std::vector;
using std::make_pair;
using std::ostringstream;

namespace google_breakpad {

#ifdef _WIN32
#define strtok_r strtok_s
#define strtoull _strtoui64
#endif

static const char *kWhitespace = " \r\n";
static const int kMaxErrorsPrinted = 5;
static const int kMaxErrorsBeforeBailing = 100;

BasicSourceLineResolver::BasicSourceLineResolver() :
    SourceLineResolverBase(new BasicModuleFactory) { }

// static
void BasicSourceLineResolver::Module::LogParseError(
   const string &message,
   int line_number,
   int *num_errors) {
  if (++(*num_errors) <= kMaxErrorsPrinted) {
    if (line_number > 0) {
      BPLOG(ERROR) << "Line " << line_number << ": " << message;
    } else {
      BPLOG(ERROR) << message;
    }
  }
}

bool BasicSourceLineResolver::Module::LoadMapFromMemory(
    char *memory_buffer,
    size_t memory_buffer_size) {
  linked_ptr<Function> cur_func;
  int line_number = 0;
  int num_errors = 0;
  char *save_ptr;

  // If the length is 0, we can still pretend we have a symbol file. This is
  // for scenarios that want to test symbol lookup, but don't necessarily care
  // if certain modules do not have any information, like system libraries.
  if (memory_buffer_size == 0) {
    return true;
  }

  // Make sure the last character is null terminator.
  size_t last_null_terminator = memory_buffer_size - 1;
  if (memory_buffer[last_null_terminator] != '\0') {
    memory_buffer[last_null_terminator] = '\0';
  }

  // Skip any null terminators at the end of the memory buffer, and make sure
  // there are no other null terminators in the middle of the memory buffer.
  bool has_null_terminator_in_the_middle = false;
  while (last_null_terminator > 0 &&
         memory_buffer[last_null_terminator - 1] == '\0') {
    last_null_terminator--;
  }
  for (size_t i = 0; i < last_null_terminator; i++) {
    if (memory_buffer[i] == '\0') {
      memory_buffer[i] = '_';
      has_null_terminator_in_the_middle = true;
    }
  }
  if (has_null_terminator_in_the_middle) {
    LogParseError(
       "Null terminator is not expected in the middle of the symbol data",
       line_number,
       &num_errors);
  }

  char *buffer;
  buffer = strtok_r(memory_buffer, "\r\n", &save_ptr);

  while (buffer != NULL) {
    ++line_number;

    if (strncmp(buffer, "FILE ", 5) == 0) {
      if (!ParseFile(buffer)) {
        LogParseError("ParseFile on buffer failed", line_number, &num_errors);
      }
    } else if (strncmp(buffer, "STACK ", 6) == 0) {
      if (!ParseStackInfo(buffer)) {
        LogParseError("ParseStackInfo failed", line_number, &num_errors);
      }
    } else if (strncmp(buffer, "FUNC ", 5) == 0) {
      cur_func.reset(ParseFunction(buffer));
      if (!cur_func.get()) {
        LogParseError("ParseFunction failed", line_number, &num_errors);
      } else {
        // StoreRange will fail if the function has an invalid address or size.
        // We'll silently ignore this, the function and any corresponding lines
        // will be destroyed when cur_func is released.
        functions_.StoreRange(cur_func->address, cur_func->size, cur_func);
      }
    } else if (strncmp(buffer, "PUBLIC ", 7) == 0) {
      // Clear cur_func: public symbols don't contain line number information.
      cur_func.reset();

      if (!ParsePublicSymbol(buffer)) {
        LogParseError("ParsePublicSymbol failed", line_number, &num_errors);
      }
    } else if (strncmp(buffer, "MODULE ", 7) == 0) {
      // Ignore these.  They're not of any use to BasicSourceLineResolver,
      // which is fed modules by a SymbolSupplier.  These lines are present to
      // aid other tools in properly placing symbol files so that they can
      // be accessed by a SymbolSupplier.
      //
      // MODULE <guid> <age> <filename>
    } else if (strncmp(buffer, "INFO ", 5) == 0) {
      // Ignore these as well, they're similarly just for housekeeping.
      //
      // INFO CODE_ID <code id> <filename>
    } else {
      if (!cur_func.get()) {
        LogParseError("Found source line data without a function",
                       line_number, &num_errors);
      } else {
        Line *line = ParseLine(buffer);
        if (!line) {
          LogParseError("ParseLine failed", line_number, &num_errors);
        } else {
          cur_func->lines.StoreRange(line->address, line->size,
                                     linked_ptr<Line>(line));
        }
      }
    }
    if (num_errors > kMaxErrorsBeforeBailing) {
      BPLOG(ERROR) << "Parsing symbol file stopped for there are too many error occurs.";
      break;
    }
    buffer = strtok_r(NULL, "\r\n", &save_ptr);
  }

  BPLOG(ERROR) << "total errors in parsing symbol files: " << num_errors;

  is_corrupt_ = num_errors > 0;
  return true;
}

static inline uint64_t EvaluateDwarfExpression(StackFrame* frame, MemoryRegion* memory,
        const vector<ArgLocInfo>& loc)
{
  vector<uint64_t> s;
  using namespace dwarf2reader;
  uint64_t base = frame->GetFrameBase();

  if (base == 0)
  {
    BPLOG(ERROR) << "unexpected stack frame type, or invalid stack pointer.";
  }

  uint64_t val;
  for (size_t i = 0; i < loc.size(); ++i)
  {
      const ArgLocInfo& ai = loc[i];
      const unsigned char op = ai.op;

      if (op >= DW_OP_reg0 && op <= DW_OP_reg31)
      {
        int index = op - DW_OP_reg0;
        val = frame->GetRegValue(index);
      }
      else if (op == DW_OP_fbreg)
      {
        val = (long long)base + (long long)ai.locValue1;
      }
      else if (op == DW_OP_addr)
      {
        val = ai.locValue1;
      }
      else if (op == DW_OP_regX)
      {
        val = frame->GetRegValue(ai.locValue1);
      }
      else if (op >= DW_OP_breg0 && op <= DW_OP_breg31)
      {
        int index = op - DW_OP_breg0;
        val = (long long)frame->GetRegValue(index) + (long long)ai.locValue1;
      }
      else if (op == DW_OP_deref)
      {
        if (s.empty()) return 0;

        uint64_t addr = s.back();
        s.pop_back();
        if (!memory->GetMemoryAtAddress(addr, &val)) return 0;
      }
      else if (op >= DW_OP_lit0 && op <= DW_OP_lit31)
      {
        val = op - DW_OP_lit0;
      }
      else if (op == DW_OP_const1u || op == DW_OP_const2u ||
              op == DW_OP_const4u || op == DW_OP_const8u)
      {
        val = ai.locValue1;
      }
      else if (op == DW_OP_const1s)
      {
        char c = (ai.locValue1 & 0xff);
        val = (uint64_t)c;
      }
      else if (op == DW_OP_const2s)
      {
        short c = (ai.locValue1 & 0xffff);
        val = (uint64_t)c;
      }
      else if (op == DW_OP_const4s)
      {
        int c = (ai.locValue1 & 0xffffffff);
        val = (uint64_t)c;
      }
      else if (op == DW_OP_const8s)
      {
        val = ai.locValue1;
      }
      else if (op == DW_OP_dup)
      {
        if (s.empty()) return 0;

        s.push_back(s.back());
      }
      else if (op == DW_OP_drop)
      {
        if (s.empty()) return 0;

        s.pop_back();
      }
      else if (op == DW_OP_pick)
      {
        if (s.empty()) return 0;

        size_t index = s.size() - 1;
        if (index < ai.locValue1) return 0;

        index = index - ai.locValue1;
        s.push_back(s[index]);
      }
      else if (op == DW_OP_over)
      {
        if (s.size() < 2) return 0;

        s.push_back(s[s.size() - 2]);
      }
      else if (op == DW_OP_swap)
      {
        if (s.size() < 2) return 0;

        std::swap(s[s.size() - 1], s[s.size() - 2]);
      }
      else if (op == DW_OP_rot)
      {
        if (s.size() < 3) return 0;

        std::swap(s[s.size() - 1], s[s.size() - 3]);
        std::swap(s[s.size() - 1], s[s.size() - 2]);
      }
      else if (op == DW_OP_deref_size || op == DW_OP_xderef
              || op == DW_OP_xderef_size)
      {
          // TODO
          return 0;
      }

      s.push_back(val);
  }

  if (!s.empty()) return s.back();

  return 0;
}

static void ReadFuncParams(StackFrame* frame, const vector<FuncParam>& params,
    MemoryRegion* memory, vector<StackFrame::ParamInfo>& info)
{
  if (!memory || params.empty()) return;

  info.clear();
  info.reserve(params.size());

  for (size_t i = 0; i < params.size(); ++i)
  {
    StackFrame::ParamInfo param;

    param.typeName = params[i].typeName;
    param.typeSize = params[i].typeSize;
    param.paramName = params[i].paramName;

    if (param.typeSize <= 0)
    {
      info.push_back(param);
      continue;
    }

    uint64_t value;
    uint64_t addr = EvaluateDwarfExpression(frame, memory, params[i].locs);
    if (!addr)
    {
        BPLOG(ERROR) << "invalid location expression for func:"
            << frame->function_name << ", param:" << param.paramName
            << "(" << param.typeName << ")";
        continue;
    }

    if (!memory->GetMemoryAtAddress(addr, &value)) return;

    ostringstream oss;
    bool show_simple_type = false;
    if (param.typeSize % 2 == 0 && param.typeSize <= sizeof(uint64_t))
    {
      if (param.typeName.find("*") != string::npos
          || param.typeName.find("&") != string::npos)
      {
        oss << std::hex << (void*)value;
      }
      else if (param.typeName.find("float") != string::npos)
      {
        oss << *(float*)&value;
      }
      else if (param.typeName.find("double") != string::npos)
      {
        oss << *(double*)&value;
      }
      else
      {
        int bitsz = ((sizeof(uint64_t) - param.typeSize) << 3);
        uint64_t mask = ~0;

        mask <<= bitsz;
        mask >>= bitsz;

        oss << "0x" << std::hex << (value & mask);
      }

      show_simple_type = true;
    }

    if (addr)
    {
      uint8_t byte_value;
      memory->GetMemoryAtAddress(addr, &byte_value);

      if (show_simple_type) oss << ", ";

      oss << "hex:" << std::hex << (unsigned int)byte_value;
      for (int j = 1; j < param.typeSize; ++j)
      {
        memory->GetMemoryAtAddress(addr + j, &byte_value);
        oss << " " << (unsigned int)byte_value;
      }
    }

    param.value = oss.str();
    info.push_back(param);
  }
}

void BasicSourceLineResolver::Module::LookupAddress(MemoryRegion* memory, StackFrame *frame) const {
  MemAddr address = frame->instruction - frame->module->base_address();

  // First, look for a FUNC record that covers address. Use
  // RetrieveNearestRange instead of RetrieveRange so that, if there
  // is no such function, we can use the next function to bound the
  // extent of the PUBLIC symbol we find, below. This does mean we
  // need to check that address indeed falls within the function we
  // find; do the range comparison in an overflow-friendly way.
  linked_ptr<Function> func;
  linked_ptr<PublicSymbol> public_symbol;
  MemAddr function_base;
  MemAddr function_size;
  MemAddr public_address;
  if (functions_.RetrieveNearestRange(address, &func,
              &function_base, &function_size) && address >= function_base
              && address - function_base < function_size) {

    frame->function_name = func->name;
    frame->function_base = frame->module->base_address() + function_base;

    ReadFuncParams(frame, func->params, memory, frame->params);

    linked_ptr<Line> line;
    MemAddr line_base;
    if (func->lines.RetrieveRange(address, &line, &line_base, NULL)) {
      FileMap::const_iterator it = files_.find(line->source_file_id);
      if (it != files_.end()) {
        frame->source_file_name = files_.find(line->source_file_id)->second;
      }
      frame->source_line = line->line;
      frame->source_line_base = frame->module->base_address() + line_base;
    }
  } else if (public_symbols_.Retrieve(address,
                                      &public_symbol, &public_address) &&
             (!func.get() || public_address > function_base)) {
    frame->function_name = public_symbol->name;
    frame->function_base = frame->module->base_address() + public_address;
  }
}

WindowsFrameInfo *BasicSourceLineResolver::Module::FindWindowsFrameInfo(
    const StackFrame *frame) const {
  MemAddr address = frame->instruction - frame->module->base_address();
  scoped_ptr<WindowsFrameInfo> result(new WindowsFrameInfo());

  // We only know about WindowsFrameInfo::STACK_INFO_FRAME_DATA and
  // WindowsFrameInfo::STACK_INFO_FPO. Prefer them in this order.
  // WindowsFrameInfo::STACK_INFO_FRAME_DATA is the newer type that
  // includes its own program string.
  // WindowsFrameInfo::STACK_INFO_FPO is the older type
  // corresponding to the FPO_DATA struct. See stackwalker_x86.cc.
  linked_ptr<WindowsFrameInfo> frame_info;
  if ((windows_frame_info_[WindowsFrameInfo::STACK_INFO_FRAME_DATA]
       .RetrieveRange(address, &frame_info))
      || (windows_frame_info_[WindowsFrameInfo::STACK_INFO_FPO]
          .RetrieveRange(address, &frame_info))) {
    result->CopyFrom(*frame_info.get());
    return result.release();
  }

  // Even without a relevant STACK line, many functions contain
  // information about how much space their parameters consume on the
  // stack. Use RetrieveNearestRange instead of RetrieveRange, so that
  // we can use the function to bound the extent of the PUBLIC symbol,
  // below. However, this does mean we need to check that ADDRESS
  // falls within the retrieved function's range; do the range
  // comparison in an overflow-friendly way.
  linked_ptr<Function> function;
  MemAddr function_base, function_size;
  if (functions_.RetrieveNearestRange(address, &function,
                                      &function_base, &function_size) &&
      address >= function_base && address - function_base < function_size) {
    result->parameter_size = function->parameter_size;
    result->valid |= WindowsFrameInfo::VALID_PARAMETER_SIZE;
    return result.release();
  }

  // PUBLIC symbols might have a parameter size. Use the function we
  // found above to limit the range the public symbol covers.
  linked_ptr<PublicSymbol> public_symbol;
  MemAddr public_address;
  if (public_symbols_.Retrieve(address, &public_symbol, &public_address) &&
      (!function.get() || public_address > function_base)) {
    result->parameter_size = public_symbol->parameter_size;
  }

  return NULL;
}

CFIFrameInfo *BasicSourceLineResolver::Module::FindCFIFrameInfo(
    const StackFrame *frame) const {
  MemAddr address = frame->instruction - frame->module->base_address();
  MemAddr initial_base, initial_size;
  string initial_rules;

  // Find the initial rule whose range covers this address. That
  // provides an initial set of register recovery rules. Then, walk
  // forward from the initial rule's starting address to frame's
  // instruction address, applying delta rules.
  if (!cfi_initial_rules_.RetrieveRange(address, &initial_rules,
                                        &initial_base, &initial_size)) {
    return NULL;
  }

  // Create a frame info structure, and populate it with the rules from
  // the STACK CFI INIT record.
  scoped_ptr<CFIFrameInfo> rules(new CFIFrameInfo());
  if (!ParseCFIRuleSet(initial_rules, rules.get()))
    return NULL;

  // Find the first delta rule that falls within the initial rule's range.
  map<MemAddr, string>::const_iterator delta =
    cfi_delta_rules_.lower_bound(initial_base);

  // Apply delta rules up to and including the frame's address.
  while (delta != cfi_delta_rules_.end() && delta->first <= address) {
    ParseCFIRuleSet(delta->second, rules.get());
    delta++;
  }

  return rules.release();
}

bool BasicSourceLineResolver::Module::ParseFile(char *file_line) {
  long index;
  char *filename;
  if (SymbolParseHelper::ParseFile(file_line, &index, &filename)) {
    files_.insert(make_pair(index, string(filename)));
    return true;
  }
  return false;
}

BasicSourceLineResolver::Function*
BasicSourceLineResolver::Module::ParseFunction(char *function_line) {
  uint64_t address;
  uint64_t size;
  long stack_param_size;
  char *name;
  vector<FuncParam> params;

  if (SymbolParseHelper::ParseFunction(function_line, &address, &size,
                                       &stack_param_size, &name, params)) {
    return new Function(name, address, size, stack_param_size, params);
  }
  return NULL;
}

BasicSourceLineResolver::Line* BasicSourceLineResolver::Module::ParseLine(
    char *line_line) {
  uint64_t address;
  uint64_t size;
  long line_number;
  long source_file;

  if (SymbolParseHelper::ParseLine(line_line, &address, &size, &line_number,
                                   &source_file)) {
    return new Line(address, size, source_file, line_number);
  }
  return NULL;
}

bool BasicSourceLineResolver::Module::ParsePublicSymbol(char *public_line) {
  uint64_t address;
  long stack_param_size;
  char *name;

  if (SymbolParseHelper::ParsePublicSymbol(public_line, &address,
                                           &stack_param_size, &name)) {
    // A few public symbols show up with an address of 0.  This has been seen
    // in the dumped output of ntdll.pdb for symbols such as _CIlog, _CIpow,
    // RtlDescribeChunkLZNT1, and RtlReserveChunkLZNT1.  They would conflict
    // with one another if they were allowed into the public_symbols_ map,
    // but since the address is obviously invalid, gracefully accept them
    // as input without putting them into the map.
    if (address == 0) {
      return true;
    }

    linked_ptr<PublicSymbol> symbol(new PublicSymbol(name, address,
                                                     stack_param_size));
    return public_symbols_.Store(address, symbol);
  }
  return false;
}

bool BasicSourceLineResolver::Module::ParseStackInfo(char *stack_info_line) {
  // Skip "STACK " prefix.
  stack_info_line += 6;

  // Find the token indicating what sort of stack frame walking
  // information this is.
  while (*stack_info_line == ' ')
    stack_info_line++;
  const char *platform = stack_info_line;
  while (!strchr(kWhitespace, *stack_info_line))
    stack_info_line++;
  *stack_info_line++ = '\0';

  // MSVC stack frame info.
  if (strcmp(platform, "WIN") == 0) {
    int type = 0;
    uint64_t rva, code_size;
    linked_ptr<WindowsFrameInfo>
      stack_frame_info(WindowsFrameInfo::ParseFromString(stack_info_line,
                                                         type,
                                                         rva,
                                                         code_size));
    if (stack_frame_info == NULL)
      return false;

    // TODO(mmentovai): I wanted to use StoreRange's return value as this
    // method's return value, but MSVC infrequently outputs stack info that
    // violates the containment rules.  This happens with a section of code
    // in strncpy_s in test_app.cc (testdata/minidump2).  There, problem looks
    // like this:
    //   STACK WIN 4 4242 1a a 0 ...  (STACK WIN 4 base size prolog 0 ...)
    //   STACK WIN 4 4243 2e 9 0 ...
    // ContainedRangeMap treats these two blocks as conflicting.  In reality,
    // when the prolog lengths are taken into account, the actual code of
    // these blocks doesn't conflict.  However, we can't take the prolog lengths
    // into account directly here because we'd wind up with a different set
    // of range conflicts when MSVC outputs stack info like this:
    //   STACK WIN 4 1040 73 33 0 ...
    //   STACK WIN 4 105a 59 19 0 ...
    // because in both of these entries, the beginning of the code after the
    // prolog is at 0x1073, and the last byte of contained code is at 0x10b2.
    // Perhaps we could get away with storing ranges by rva + prolog_size
    // if ContainedRangeMap were modified to allow replacement of
    // already-stored values.

    windows_frame_info_[type].StoreRange(rva, code_size, stack_frame_info);
    return true;
  } else if (strcmp(platform, "CFI") == 0) {
    // DWARF CFI stack frame info
    return ParseCFIFrameInfo(stack_info_line);
  } else {
    // Something unrecognized.
    return false;
  }
}

bool BasicSourceLineResolver::Module::ParseCFIFrameInfo(
    char *stack_info_line) {
  char *cursor;

  // Is this an INIT record or a delta record?
  char *init_or_address = strtok_r(stack_info_line, " \r\n", &cursor);
  if (!init_or_address)
    return false;

  if (strcmp(init_or_address, "INIT") == 0) {
    // This record has the form "STACK INIT <address> <size> <rules...>".
    char *address_field = strtok_r(NULL, " \r\n", &cursor);
    if (!address_field) return false;

    char *size_field = strtok_r(NULL, " \r\n", &cursor);
    if (!size_field) return false;

    char *initial_rules = strtok_r(NULL, "\r\n", &cursor);
    if (!initial_rules) return false;

    MemAddr address = strtoul(address_field, NULL, 16);
    MemAddr size    = strtoul(size_field,    NULL, 16);
    cfi_initial_rules_.StoreRange(address, size, initial_rules);
    return true;
  }

  // This record has the form "STACK <address> <rules...>".
  char *address_field = init_or_address;
  char *delta_rules = strtok_r(NULL, "\r\n", &cursor);
  if (!delta_rules) return false;
  MemAddr address = strtoul(address_field, NULL, 16);
  cfi_delta_rules_[address] = delta_rules;
  return true;
}

// static
bool SymbolParseHelper::ParseFile(char *file_line, long *index,
                                  char **filename) {
  // FILE <id> <filename>
  assert(strncmp(file_line, "FILE ", 5) == 0);
  file_line += 5;  // skip prefix

  vector<char*> tokens;
  if (!Tokenize(file_line, kWhitespace, 2, &tokens)) {
    return false;
  }

  char *after_number;
  *index = strtol(tokens[0], &after_number, 10);
  if (!IsValidAfterNumber(after_number) || *index < 0 ||
      *index == std::numeric_limits<long>::max()) {
    return false;
  }

  *filename = tokens[1];
  if (!filename) {
    return false;
  }

  return true;
}

bool SymbolParseHelper::ParseFuncParam(vector<char*>& pv, vector<FuncParam>& params)
{
  char *after_number;
  size_t num_params = pv.size();
  params.reserve(num_params);

  for (int i = 0; i < num_params; ++i)
  {
    vector<char*> args;
    if (!Tokenize(pv[i], "@", 4, &args))
    {
      params.clear();
      return false;
    }

    FuncParam p;
    p.typeName = args[0];
    p.typeSize = strtoull(args[1], &after_number, 16);
    if (!after_number || *after_number) p.typeSize = 0;

    p.paramName = args[2];

    vector<char*> loc_exp;
    loc_exp.reserve(32);
    Tokenize(args[3], "$", std::numeric_limits<int>::max(), &loc_exp);
    if (loc_exp.empty()) return false;

    for (size_t j = 0; j < loc_exp.size(); ++j)
    {
        vector<char*> locs;
        Tokenize(loc_exp[j], ":", 4, &locs);
        if (locs.empty())
        {
            params.clear();
            return false;
        }

        ArgLocInfo ai;

        ai.op = strtoul(locs[0], &after_number, 16);
        if (!after_number || *after_number)
        {
          params.clear();
          return false;
        }

        if (locs.size() > 1)
        {
            ai.locValue1 = strtoull(locs[1], &after_number, 16);
            if (!after_number || *after_number) ai.locValue1 = 0;
        }

        if (locs.size() > 2)
        {
            ai.locValue2 = strtoull(locs[2], &after_number, 16);
            if (!after_number || *after_number) ai.locValue2 = 0;
        }

        p.locs.push_back(ai);
    }

    params.push_back(p);
  }
}

// static
bool SymbolParseHelper::ParseFunction(char *function_line, uint64_t *address,
                                      uint64_t *size, long *stack_param_size,
                                      char **name, vector<FuncParam>& params) {
  // FUNC <address> <size> <stack_param_size> <name>
  assert(strncmp(function_line, "FUNC ", 5) == 0);
  function_line += 5;  // skip prefix

  vector<char*> segments;
  Tokenize(function_line, "#", 3, &segments);
  if (segments.empty()) return false;

  vector<char*> tokens;
  if (!Tokenize(segments[0], kWhitespace, 4, &tokens)) {
    return false;
  }

  char *after_number;
  *address = strtoull(tokens[0], &after_number, 16);
  if (!IsValidAfterNumber(after_number) ||
      *address == std::numeric_limits<unsigned long long>::max()) {
    return false;
  }
  *size = strtoull(tokens[1], &after_number, 16);
  if (!IsValidAfterNumber(after_number) ||
      *size == std::numeric_limits<unsigned long long>::max()) {
    return false;
  }
  *stack_param_size = strtol(tokens[2], &after_number, 16);
  if (!IsValidAfterNumber(after_number) ||
      *stack_param_size == std::numeric_limits<long>::max() ||
      *stack_param_size < 0) {
    return false;
  }
  *name = tokens[3];

  if (segments.size() == 3)
  {
    vector<char*> argsArray;
    size_t num_params = strtoul(segments[1], &after_number, 16);
    if (!after_number || *after_number) return true;

    if (!Tokenize(segments[2], "#", num_params, &argsArray)) return true;

    ParseFuncParam(argsArray, params);
  }

  return true;
}

// static
bool SymbolParseHelper::ParseLine(char *line_line, uint64_t *address,
                                  uint64_t *size, long *line_number,
                                  long *source_file) {
  // <address> <size> <line number> <source file id>
  vector<char*> tokens;
  if (!Tokenize(line_line, kWhitespace, 4, &tokens)) {
      return false;
  }

  char *after_number;
  *address  = strtoull(tokens[0], &after_number, 16);
  if (!IsValidAfterNumber(after_number) ||
      *address == std::numeric_limits<unsigned long long>::max()) {
    return false;
  }
  *size = strtoull(tokens[1], &after_number, 16);
  if (!IsValidAfterNumber(after_number) ||
      *size == std::numeric_limits<unsigned long long>::max()) {
    return false;
  }
  *line_number = strtol(tokens[2], &after_number, 10);
  if (!IsValidAfterNumber(after_number) ||
      *line_number == std::numeric_limits<long>::max()) {
    return false;
  }
  *source_file = strtol(tokens[3], &after_number, 10);
  if (!IsValidAfterNumber(after_number) || *source_file < 0 ||
      *source_file == std::numeric_limits<long>::max()) {
    return false;
  }

  // Valid line numbers normally start from 1, however there are functions that
  // are associated with a source file but not associated with any line number
  // (block helper function) and for such functions the symbol file contains 0
  // for the line numbers.  Hence, 0 should be treated as a valid line number.
  // For more information on block helper functions, please, take a look at:
  // http://clang.llvm.org/docs/Block-ABI-Apple.html
  if (*line_number < 0) {
    return false;
  }

  return true;
}

// static
bool SymbolParseHelper::ParsePublicSymbol(char *public_line,
                                          uint64_t *address,
                                          long *stack_param_size,
                                          char **name) {
  // PUBLIC <address> <stack_param_size> <name>
  assert(strncmp(public_line, "PUBLIC ", 7) == 0);
  public_line += 7;  // skip prefix

  vector<char*> tokens;
  if (!Tokenize(public_line, kWhitespace, 3, &tokens)) {
    return false;
  }

  char *after_number;
  *address = strtoull(tokens[0], &after_number, 16);
  if (!IsValidAfterNumber(after_number) ||
      *address == std::numeric_limits<unsigned long long>::max()) {
    return false;
  }
  *stack_param_size = strtol(tokens[1], &after_number, 16);
  if (!IsValidAfterNumber(after_number) ||
      *stack_param_size == std::numeric_limits<long>::max() ||
      *stack_param_size < 0) {
    return false;
  }
  *name = tokens[2]; 

  return true;
}

// static
bool SymbolParseHelper::IsValidAfterNumber(char *after_number) {
  if (after_number != NULL && strchr(kWhitespace, *after_number) != NULL) {
    return true;
  }
  return false;
}

}  // namespace google_breakpad
