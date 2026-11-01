# Code Style Guide

> AI agents: read this file completely before project-code analysis/edits, after context loss, and after a user update. Then follow 25.5: one chunk at a time; searches and green builds never replace full reads or verification passes.

**K&R / 1TBS + Systems Hungarian + Explicit Systems C++**, with a JavaScript profile. Self-contained specification for AI agents: style rules, defaults, and exceptions are defined here. Section references are local to this document; resolve conflicts using 2.2's precedence. Example APIs/paths are not repository requirements.

## Quick reference for AI agents

Navigation only, not a reduced checklist. Apply every relevant rule; formatters never override the guide.

## Table of contents

| Area | Sections |
| --- | --- |
| Principles/profile | [1. Principles](#1-core-principles) · [2. Scope](#2-scope) · [3. Character](#3-overall-character-of-the-style) |
| JavaScript | [2.6. JS/API examples](#26-javascript-packages-and-api-examples) · [Direct imports](#direct-named-imports) |
| Adoption | [2.7. Other projects](#27-adopting-this-guide-in-another-project) |
| Layout | [4. Files](#4-file-organization) · [5. Braces](#5-braces) · [6. Indentation/phases](#6-indentation-and-vertical-rhythm) · [7. Expressions/initialization](#7-spacing-and-micro-style) |
| Naming/types | [8. Naming](#8-naming) · [Operation words](#complete-words-and-operation-names) · [9. Types](#9-types-declarations-and-qualifiers) |
| Implementation | [10. Classes](#10-class-and-struct-organization) · [11. Bodies/deduction](#11-function-body-style) |
| Contracts | [12. Errors](#12-error-handling) · [13. APIs](#13-api-design) |
| Systems | [14. Low-level](#14-low-level-and-platform-aware-code) · [15. Resources](#15-resource-management) · [16. Synchronization](#16-synchronization) |
| Utilities | [17. STL](#17-stl-profile) · [18. Compile-time/header-only](#18-compile-time-and-header-only-utilities) |
| Explanations | [19. Comments](#19-comments) · [20. Dislikes](#20-what-this-style-dislikes) · [21. Templates](#21-canonical-templates) |
| Interpretation | [22. MUST/SHOULD/MAY](#22-rules-in-must--should--may-form) · [23. Summary](#23-one-line-summary) · [24. Non-goals](#24-non-goals) |
| Verification | [25. Maintenance/docs/checks](#25-portable-maintenance-and-verification-rules) · [25.5. Chunk workflow](#255-ai-agent-editing-rules) |

## 1. Core Principles

Make behavior, ownership, dependencies, failures, and domain-critical details readable top-to-bottom. Prefer explicit contracts and flat control flow. Brevity never outranks clarity, ABI/layout, diagnostics, defined behavior, or compile-time predictability.

## 2. Scope

Apply to project-owned code, libraries, tools, packages, templates, generated source, and executable documentation.

### 2.1. Language Baseline

Examples assume C++20; earlier profiles must deliberately replace unavailable facilities, not emulate them through unrelated macros or unsafe casts. Apply C++/JavaScript rules only to their languages; shared rules only where syntax permits (no tabs in space-only formats).

Example APIs, paths, platforms, and tools are illustrative, not dependencies. Ellipses/omitted declarations show local shapes; executable examples must be complete and valid.

### 2.2. Rule Precedence

Highest first: language correctness/defined behavior → ABI/external API/wire/file contracts → security/lifetime → documented project-local rules → detailed numbered rules → summaries/examples.

**MUST**: required unless a higher contract prevents it. **SHOULD/prefer/recommended**: default; deviation needs a documented local reason. **MAY/allowed**: permitted, not universally recommended. A documented compiler/platform exception may accept a narrow portability risk, never undefined behavior or an incorrect ABI.

### 2.3. Legacy Code and Migration

New files follow the guide. Focused behavioral patches preserve a legacy file's single convention, or migrate the whole file in a dedicated style change; never mix conventions. Do not combine behavior changes and repository-wide restyling unless requested. Preserve public APIs, callbacks, serialization, ABI layouts, CLI flags, and generated contracts unless a breaking migration is authorized.

### 2.4. Universal and Domain-Specific Rules

Formatting, naming, organization, errors, ownership, comments, and verification are shared. Apply low-level/platform/compile-time tools only where the domain requires them; permission is not a reason to introduce them.

### 2.5. Repository Ownership and Compatibility Exceptions

Classify each path when current: project-owned (in scope); vendored (read-only without dependency-update authorization); protected compatibility (inspect contracts, edit only with authorization); generated-only (edit the available owning generator); binary fixture/build artifact (no text formatting).

Maintain a local ownership/output map. Audits/formatters exclude vendored/protected paths by default; diagnostic inclusion grants no write authority. Fix owned integration when an excluded dependency triggers a diagnostic. Never silently overwrite generated/user content.

Forbid project identifiers containing `__` or starting with underscore + uppercase, except the exact canonical guard (4.1), boundary `_TAG` typedefs (9.5), or required external ABI spelling. Document each narrow exception in the compatibility profile; never broaden it. External compiler/platform identifiers stay exact.

### 2.6. JavaScript, Packages, and API Examples

Shared rules cover JavaScript source, packages, scripts, helpers, templates, and executable docs; C++-only constructs do not transfer.

- Unreassigned binding: `const`; reassigned: `let`. Neither object immutability nor uppercase naming follows from `const`.
- All variables/value parameters use 8's Systems Hungarian + PascalCase: `bReady`, signed/sentinel `nResult`, nonnegative `unCount`, `strName`, `vecItems`, callback parameter `fnCallback`. Objects and named arrow/function-expression bindings have no category prefix: `Manager`, `Predicate`, even when used as callbacks. Module implementation state keeps `g_`, including `g_Manager`. Select categories from actual contracts; named functions/types are PascalCase.
- Preserve established UPPER_CASE (= UPPER_SNAKE_CASE) exports, including runtime-acquired `ADDON_ROOT`. Substantial destructuring/array/object inventories may be multiline: one entry per line, trailing comma. Simple calls follow 6.4.
- Synopsis/table/callback parameter names follow implementation naming and actual types, consistently throughout docs. Numeric nonnegative address + string path: `netchannel.RequestFile(unChannelAddress, strFilePath)`; string IDs + object: `strPlayerName`, `strSteamID64`, `Manager`. IDs are not necessarily numeric. Executable examples use declared bindings.
- Preserve exact exports, properties, lifecycle and serialized keys; placeholders do not rename them. Verify current implementation and branch/platform availability.
- Native-address names/descriptions use `Address`/`address`, not `Pointer`/`pointer`; preserve external identifiers and the technical C++ term “pointer”. Numeric addresses are not JavaScript object references.
- Preserve evaluation order, short-circuiting, and operand results; no formatting-driven coercion or replacement of explicit null/type checks. Preserve actual failure results; C++ `return {};` is not a JavaScript failure rule.
- Imports/setup are not C++ dynamic globals; do not defer/reorder them to imitate C++.

```js
const {
	ADDON_ROOT,
	AUTH_CONFIG_ROOT,
	AUTH_PARENTS_FILE,
} = require("paths");

const g_vecRuntimePaths = [
	ADDON_ROOT,
	AUTH_CONFIG_ROOT,
	AUTH_PARENTS_FILE,
];
```

#### Direct Named Imports

Use a unique PascalCase/UPPER_CASE export directly, without automatic scope/category aliases:

```js
const { GetRawModule } = require("internal/runtime");

const g_RawModule = GetRawModule("memory");
```

Alias only for a real lexical collision or shadowing of a required binding: `GetRawModule: g_fnGetRawModule`, or a more specific unique role such as `g_fnGetRuntimeRawModule`; non-functions use their category. Unrelated scopes, comments, strings, and property keys are not collisions; do not manufacture one.

Removing aliases updates all bound uses, closures, and shorthand members while preserving public keys, exports, receivers, and loading order. No text-only replacement, repeated `require`, or forwarding wrapper.

New exports use unique semantic PascalCase or established uppercase constants. Preserve external/compatibility spelling; no silent API rename or style-only alias. This contract exception does not change ordinary variable/parameter naming. Named ES imports follow the same collision rule with `as`; preserve the module system.

### 2.7. Adopting This Guide in Another Project

Record repository facts in an adoption profile: languages/versions/JavaScript runtime/module system; platforms/compilers/architectures/configurations/dependency versions; ownership map; naming/ABI/serialization/lifecycle contracts; justified exceptions and rule IDs; actual generation/format/lint/test/docs/build commands.

Select applicable rules without weakening safety or inventing verification. Derive facts from the actual repository, not illustrative APIs, another project's names, engine branches, deployment paths, host addresses, or commands.

## 3. Overall Character of the Style

Prefer explicit, auditable systems code over abstraction for its own sake. Verbose signatures, C-compatible types, and low-level tools are valid when they expose needed control; their constraints remain in 12–18.

## 4. File Organization

### 4.1. File Header

Use one project policy: guard, `#pragma once`, or deliberately both; public/portable libraries should retain a guard. Needed compiler pragmas are allowed.

Canonical guard: `_<PROJECT><MODULE><FILE>_H_`, uppercase concatenated roles without separators, e.g. `_SOMELIBSOMEHEADER_H_`. Preserve complete collision-resistant spelling in `#ifndef`, `#define`, and closing `#endif // _SOMELIBSOMEHEADER_H_`. Reserved-name exceptions require 2.5's profile.

### 4.2. Include Order

Default: own header → General → C → C++ → STL → third-party → project. One blank line between groups; alphabetize within dependency/platform constraints. Required labels/classification:

| Label | Responsibility / examples |
| --- | --- |
| `// General` | OS/compiler/SDK: `Windows.h`, `sys/mman.h`, vendor SDK |
| `// C` | C-runtime/platform-C spelling: `conio.h`, `fcntl.h`, `io.h` |
| `// C++` | Core runtime/C wrappers: `cerrno`, `clocale`, `cstdarg`, `cstddef`, `cstdint`, `cstdio`, `utility` |
| `// STL` | Generic facilities: `algorithm`, `array`, `cctype`, `cwctype`, `limits`, `memory`, `new`, `string`, `type_traits`, `unordered_map`, `vector` |
| `// Third-party` | Other dependencies; configured blocks follow 4.3 |
| `// Project` | Other project headers |

Classify unlisted headers by closest responsibility consistently; no alternative labels (`// Standard`, `// Runtime`, etc.).

Translation unit: own header on line 1, blank line 2. Without an own header, blank line 1, includes from line 2. Nothing precedes that slot. Own-header-first verifies self-sufficiency.

Keep platform configuration in General immediately around its include, then dependent SDK headers. Preserve caller-defined values; undefine only file-owned macros.

### 4.3. Include-Time Configuration Defines

Dependency configuration is the narrow pre-include exception: actual dependency-name comment → define → configured include → cleanup after final required use. No unrelated declarations; no generic `// Third-party` or invented `DependencyConfig` label.

```cpp
// Dependency
#define DEPENDENCY_USE_FEATURE
#include "Dependency.h"
#undef DEPENDENCY_USE_FEATURE
```

Remove private temporary macros; preserve documented public include contracts and caller definitions.

### 4.4. Declaration and Dependency Order

Default: includes/configuration → temporary platform macros → aliases/traits/enums/layout records → constants/tables → dependent types → constant/trivial file state → leaf helpers → lower-to-higher implementations → orchestration/entry points → public macro facades → cleanup.

Dependency order wins: define project types/aliases/constants/tables/helpers before use; a constexpr table-producing leaf may directly precede its table. Member definitions normally follow declarations unless a private dependency moves earlier.

No prototypes, declaration-only aliases, or stubs to bypass reorderable dependencies. Only minimum public-header incomplete-type contracts, unavailable external ABI declarations, and genuine recursive interfaces qualify. Keep external declarations in one documented compatibility header with exact types/calling convention and the reason no SDK header supplies them.

Prefer constant initialization (`constexpr`/`constinit` where applicable). Avoid non-trivial dynamic globals; unavoidable cases document order and never depend on cross-translation-unit initialization.

### 4.5. Section Banners

Stable responsibilities, not every helper. Separator after `// `: exactly 64 `=` for top-level file/namespace sections, or 64 `-` for ordinary sections; same width above/below the label.

```cpp
// ================================================================
// Parser
// ================================================================

// ----------------------------------------------------------------
// Memory
// ----------------------------------------------------------------
```

### 4.6. Header-Only Utility Layout

Header/includes/configuration → temporary compiler macros → implementation namespace → aliases/fixed types → constants/tables → leaf helpers → state/classes/mid-level algorithms → high-level constexpr/consteval APIs → optional public macro facade → cleanup → closing guard. Apply 4.4/10.3; typed implementation, thin macros.

### 4.7. Preprocessor Hygiene

- Module-specific helper names, never generic `FORCE_INLINE`/`MIN`/`MAX`/`HASH`. Parenthesize arguments/replacement expressions where needed; align multiline macros.
- Prefer typed constants/functions/templates when they preserve the contract. Established one-value macros may remain for local/CLI/ABI/platform contracts.
- Facades follow implementation; temporary helpers stay near use and are undefined immediately after final use. Reserved names: only 2.5/9.5.
- Configure platform headers or narrowly undefine known collisions (`LoadImage`, `GetObject`, `CreateEvent`, `min`, `max`); qualification does not prevent rewriting.
- No blank line before `#elif`, `#else`, `#endif`; one may follow `#endif` to separate phases.
- An established direct cleanup macro may contain one guard + one cleanup action; do not normalize it with `do { ... } while (false)`. Invoke only as a standalone statement inside braces, never an expression/unbraced body. Arguments must be stable, side-effect-free values/lvalues: no calls, increments/decrements, assignments, commas, or ownership transfers; evaluation may repeat.
- More branching/actions, a return, or ownership transfer requires a typed helper/RAII owner.

### 4.8. Internal Linkage

Prefer an anonymous namespace for grouped translation-unit-private C++ entities; `static` only when C linkage/platform signature/established convention is clearer. Do not wrap a lone helper without useful grouping or required visibility restriction; keep it at file scope. Never use anonymous namespaces in headers. Close with `} // namespace`.

## 5. Braces

### 5.1. Main Rule

K&R / 1TBS: opening brace on the controlling/function/type/namespace line. Constructor-list exception: complete signature + colon on one line; each initializer on its own following line, one extra tab, comma except after the last; opening brace alone after the list:

```cpp
SomeType::SomeType(void* const pAddress, const std::size_t unSize) :
	m_pAddress(pAddress),
	m_unSize(unSize)
{
}
```

### 5.2. Closing Brace

`}` occupies its own aligned line; attach `else`, `catch`, do-loop `while`, or required delimiter/semicolon as syntax permits. Spacing: 6.2.

### 5.3. Braces Are Mandatory

Brace every `if`/`else`/`for`/`while` body, even one statement.

### 5.4. Switch Case Indentation

Indent labels one level inside `switch`, bodies one more. Labels stand alone except an optional opening scope brace; one blank line between non-empty cases.

```cpp
switch (KindValue) {
	case Kind::A:
		return 1;

	case Kind::B: {
		const int nValue = Compute();
		return nValue;
	}

	default:
		return 0;
}
```

## 6. Indentation and Vertical Rhythm

### 6.1. Indentation

Tabs for source/scripts; spaces only for local table/macro/bit-layout alignment. Markdown prose uses spaces; fences follow their language (2.1).

### 6.2. Blank Lines

Exactly one blank line between completed phases/responsibilities/failure modes; never two consecutive blank lines in source/scripts.

| Boundary | Blank line |
| --- | --- |
| Completed statement → independent `if` | Yes |
| One local declaration/assignment → direct consumption/validation/iteration (`if`, `for`, call/assignment), even with later uses | No: one phase |
| Final single action → simple one-line early `return` inside a branch | No: includes diagnostics, calls, assignments, declarations, cleanup, any simple return result |
| Completed main-path work/output commit → final return | Yes; not the early-exit exception |
| Standalone `}`, including with a comment → ordinary statement/declaration/expression or independent `while` | Yes |
| `}` → another `}`, attached `else`/`catch`/do-`while`, required delimiter, preprocessor directive | No inserted separator |
| Constant/table/layout → first `static_assert` | Yes; related assertions stay together |
| Ordinary locals → stream declaration → first stream write | Yes at both boundaries |

No extra separator before a block/case's first `if`, an `else if`, or a documented compact cleanup phase. Never duplicate an existing blank line; distinguish independent `while` from do-loop continuation. Preprocessor boundaries: 4.7.

If an attached outer alternative starts a new phase, put one blank line **before** `} else if`/`} else`, possibly after a nested block or final statement; never between `}` and `else`.

Group local constexpr declarations first in the function; blank line before ordinary statements. A terminal early-exit cleanup sequence may remain dense.

```cpp
if (strName.empty()) {
	ThrowError(pIsolate, kInvalidNameErrorMessage);
	return false;
}

strValueOut = std::move(strValue);

return true;
```

### 6.3. Line Length

No arbitrary column limit. Wrap for auditability, nesting, or distinct logical parts, not length alone; information-rich long signatures are valid.

### 6.4. One-Line Expression Shape

- Complete C/C++ signatures, including all parameters, stay on one physical line; ordinary opening brace shares it (constructor exception: 5.1).
- Keep scannable calls/constructor calls, ternaries, boolean returns, stream chains, and initializer records on one line. Names/constants/members/casts/short expressions are simple arguments; do not split ordinary atomic operations, move/forward, getters/setters, logging, or algorithms with a named predicate for alignment. A long format string alone is not a reason.
- Wrap genuinely complex nested payloads/traversals, lambdas/aggregates, or conditions. Break after opening `(` or a clear outer boundary; one extra tab, not space-alignment. Keep short related arguments together and final `);` on the last argument line unless that argument is multiline/block-shaped.
- Long boolean/arithmetic expressions may break at top-level operators. Keep simple calls/comparisons intact, nested grouping explicit (7.5); wrapping never adds decorative parentheses.

```cpp
State.m_unActiveWorkerSlot.store(kInvalidWorkerSlot, std::memory_order_release);
CloseOperationHandle(pSlot->m_nOwnerHandle.exchange(-1, std::memory_order_acq_rel));
```

### 6.5. Streams and Generated Text

For multiline generated text: stream alone, each logical line/fragment on its own `<<` line. Emit blank lines separately, not inside leading `"\n...text...\n"`; a final `"\n\n"` chunk may terminate the chain. Generated code follows source rules.

```cpp
OutputStream
	<< "#pragma once\n"
	<< "\n"
	<< "#ifndef " << szGuardName << "\n"
	<< "#define " << szGuardName << "\n";
```

## 7. Spacing and Micro-Style

### 7.1. Space Before `{`

One space before opening `{`.

### 7.2. Space After Keywords

One space after control keywords: `if (...)`, `for (...)`, `while (...)`, `switch (...)`.

### 7.3. Inside Parentheses

No inner spaces: `if (pAddress && unSize)`.

### 7.4. Around Operators and Punctuation

Space around ordinary binary/assignment operators; unary operators attached. No space before `, ; ) ]`; one after ordinary commas. Non-empty one-line initializer/object/destructuring braces have inner spaces (`{ GetRawModule }`); empty braces are `{}`. Initialization semantics still follow 7.9.

### 7.5. Extra Parentheses

Same rules for one-line/wrapped C++ and JavaScript:

- Group each comparison operand of `&&`/`||`, including strict equality, and nested logical groups; preserve arithmetic/bitwise grouping.
- No decorative pairs on flags, members, pointer checks, calls, or unary checks: `!MatrixLike.HasMemoryAddress()`, not `(!MatrixLike.HasMemoryAddress())`.
- Control/call parentheses delimit a sole comparison: `if (unCount == 0)`, not `if ((unCount == 0))`. Compound negation keeps grouping: `!(bReady && bActive)`.
- No decorative standalone arithmetic grouping; group arithmetic within a comparison: `unEntryOffset > (unLimit - 5)`.
- Ternary: simple condition/arms unwrapped; comparison/logical/arithmetic condition grouped; arithmetic/compound-logical arms grouped as wholes: `? (A && B) : C`, not `? ((A) && (B)) : (C)`. Group the whole ternary only as another operator's operand, not merely for assignment/return/argument use; preserve necessary inner arithmetic grouping.
- Never remove call-argument or macro-protection parentheses. Use `std::max(...)`/`std::min(...)`, never `(std::max)(...)`/`(std::min)(...)`; fix collisions under 4.7.
- Preserve operands, order, short-circuiting, and result semantics.

```js
if ((g_HookType === null) || (typeof g_HookType !== "object") || Array.isArray(g_HookType)) {
	return false;
}
```

### 7.6. Numeric Literals

Decimal: ordinary counts/time/quantities. Hexadecimal: addresses/masks/offsets/opcodes/bytes/layouts, uppercase digits. Binary: meaningful bit patterns. Octal: required external conventions. Use helpful digit separators.

No decorative integer suffixes (`U`, `UL`, `ULL`, `L`, variants). Allow only locally documented type/range/overload/shift/ABI/constant-evaluation needs; prefer a named typed constant or explicit construction when clearer. Floating precision/external spellings are separate semantic cases.

Typed scalar arithmetic operands prefer `std::uint64_t(1) << 63`, not one-expression braces. Give repeated values, limits, masks, offsets, timeouts, and protocol constants semantic constexpr names.

### 7.7. Initialization and Null Values

Prefer `{}` for record/storage zero/default/value initialization and required narrowing rejection. Ordinary booleans use `= false`, `= true`, or a source expression, not empty braces/parentheses. Constructor lists and aggregate/generic initialization retain their semantics (10.3).

Use `nullptr`, `false`, numeric `0`; `NULL`/`TRUE`/`FALSE` only for required/helpful external-signature consistency. Never construct non-trivial objects with `memset`; raw buffers/verified trivial layouts may be zeroed.

### 7.8. Initialization Before Use

Every variable/address/array/record/object/member needs a defined state before any read/call/dereference/use. Prefer declaration-time initialization. Otherwise every path must establish and check state before use. Every successful constructor establishes all members via required initialization, permitted body assignment, or valid member construction. Constructor-free ABI records initialize at use (10.4).

### 7.9. Copy and Conversion Initialization

Use `Type Name = Expression;` whenever valid and semantics-preserving: copies, non-explicit conversions, factories/calls, handles, strings, containers, wrappers, casts, members, indexed/dereferenced values. Not `Type Name { Expression };` or `Type Name(Expression);`.

Direct/list initialization remains only for required explicit constructors, narrowing rejection, genuine aggregates/initializer lists, or different overload/language semantics. Empty state: 7.7; constructor lists: 10.3; type deduction independently: 11.4.

```cpp
v8::Local<v8::Object> const LocalShatterSurfaceObject = v8::Object::New(pIsolate);
std::string const strName = szName;
bool bHasKeyName = false;
```

### 7.10. Empty Return Value Initialization

For an explicitly typed, empty value-initialized result use `return {};`, not `return Type {};`/`return Type();`. Retain a required expression only if deduction or constructor semantics make `return {};` invalid/different. Not a JavaScript failure convention.

## 8. Naming

### 8.1. General Principle

All project-owned C++/JavaScript variables/value parameters (locals, fields, globals/static state, callbacks, lambdas, loops, generated code, docs) use **scope + most-specific category + PascalCase role**. Categories come from actual types/contracts, not names; references retain the value category and `auto` changes nothing.

Objects/records/wrappers/generic values with no specific category use PascalCase, **no `obj`**: `Options`, `Manager`, `g_Manager`, `m_State`. Object addresses still use `pManager`. Named lambdas/JS arrow or function-expression objects use `Predicate`/`NeedRegister`, **no `fn` even as callbacks**; scope prefixes remain.

Named functions/methods/types/namespaces: PascalCase. Exceptions: constexpr `k` (8.6), uppercase macros/established exports, direct imports (2.6), exact external/ABI/serialization names. Unprefixed camelCase is forbidden; lowercase categories in `strPlayerName` are required. Grammar placeholders, keywords, paths, and prose are not variable names. Use semantic, stable, complete roles without repeating full type names.

### 8.2. Basic Prefixes

| Prefix | Category |
| --- | --- |
| `m_` / `g_` / `s_` | Member / namespace-file mutable state / function-local static state |
| `k` | Named constexpr object only |
| `n` / `un` | Signed / unsigned integer, size, count, index, identifier |
| `b` / `fl` / `dbl` | Bool / float / double |
| `h` | OS/library handle |
| `ch` / `wch` | Narrow / wide character |
| `sz` | Fixed C-string array storage/reference |
| `str` / `wstr` / `strv` | Owning string / wide string / non-owning string view |
| `vec` / `arr` / `spn` | Dynamic sequence / fixed array / non-owning contiguous span |
| `map` / `set` | Associative map / set |
| `it` | Role-named iterator |
| `fn` | Function pointer alias/value or callback parameter; not named lambda |
| `p` | Address of object/class/struct/union/enum/custom/opaque type or void |
| `pn` / `pun` / `pb` / `pfl` / `pdbl` / `pch` / `pwch` | Address of corresponding primitive scalar/non-string character storage |
| `psz` | Address of null-terminated string or character range within one |

Each extra indirection adds `p`: `ppunCountOut`, `ppbEnabledOut`, `ppszNameOut`, `unsigned int***` → `pppun...`; custom/opaque addresses use only repeated `p` (`ppRecordOut`). Never drop primitive categories (`punCount`, not `pCount`) or invent custom-type abbreviations. Literal-backed/walking string addresses stay `psz`, not array-only `sz`.

Avoid bare `it`, `i`, `j`, `buf`, `ctx`, `idx` when a complete role is practical. Preserve external ABI/schema/CLI spellings and standard terms; these implementation categories do not rename exports.

#### Containers, Iterators, and Views

Specific representation takes precedence over object fallback in both languages:

| Prefix | Category / representative type | Example |
| --- | --- | --- |
| `it` | Iterator, including const and reverse iterators | `itRecord`, `itReverseRecord` |
| `vec` | Dynamic sequence / vector: `std::vector`, dynamic JavaScript arrays | `vecPlayers` |
| `arr` | Fixed array: raw arrays, `std::array` | `arrBytes` |
| `deq` | Double-ended queue: `std::deque` | `deqPendingRequests` |
| `lst` | List: `std::list` | `lstRequests` |
| `flst` | Singly linked / forward list: `std::forward_list` | `flstEntries` |
| `set` | Set: `std::set`, JavaScript `Set` | `setNames` |
| `mset` | Multiset: `std::multiset` | `msetScores` |
| `uset` | Unordered set: `std::unordered_set` | `usetIdentifiers` |
| `umset` | Unordered multiset: `std::unordered_multiset` | `umsetValues` |
| `map` | Associative map: `std::map`, JavaScript `Map` | `mapPlayers` |
| `mmap` | Multimap: `std::multimap` | `mmapHandlers` |
| `umap` | Unordered map: `std::unordered_map` | `umapPlayers` |
| `ummap` | Unordered multimap: `std::unordered_multimap` | `ummapHandlers` |
| `stk` | Stack adaptor: `std::stack` | `stkFrames` |
| `que` | Queue adaptor: `std::queue` | `queRequests` |
| `pque` | Priority-queue adaptor: `std::priority_queue` | `pqueTasks` |
| `spn` | Non-owning contiguous span: `std::span` | `spnData` |
| `bset` | Fixed bitset: `std::bitset` | `bsetFeatures` |

Catalog: C++20 container families/adaptors, iterators, spans, bitsets; no mandate to introduce types or upgrade the language baseline. Strings/views retain separate categories. Other libraries use the closest contract-matching category or document one in the adoption profile. A prose “list” does not imply list storage; dynamic JS arrays remain `vec`, including `g_vecRuntimePaths`. Scope composes normally: `m_deqPendingRequests`, `g_umapPlayers`, `s_queRequests`.

### 8.3. Extended Member Prefixes

Use the same categories/recursive indirection after `m_`: `m_State`, `m_unSize`, `m_punSize`, `m_ppszNameOut`, `m_fnCallback`; lambda member `m_Predicate`. No member-only scheme.

### 8.4. Type Names

Ordinary C++ classes: PascalCase. ABI/layout types may use UPPER_CASE/C-style names (`LINK_DATA`, `RAW_CONTEXT64`).

### 8.5. Function and Method Names

Verb-based PascalCase: Get/Set/Find/Encode/Decode/Allocate/Initialize/Open/Close/Load/Save/Reset/Dump families.

#### Complete Words and Operation Names

Full English words; do not split linguistic un/re/de/pre/post prefixes. Correct: `Initialize`, `Deinitialize`, `Reinitialize`, `Allocate`, `Deallocate`, `Unload`, `Unlock`, `Unregister`, `Uninstall`, `Reload`, `Rebuild`, `Readjust`, `Relink`, `Reopen`, `Reapply`, `Preload`, `Postprocess`, `Reset`, `ResetPendingOperationSlot`, `Resettable`, `IsPendingOperationSlotResettable`. Incorrect: `Init`, `Alloc`, `Deinit`, `Dealloc`, `UnLoad`, `ReLoad`, `ReSet`, `ReSettable`.

Capitalize actual next words: `ReloadConfiguration`, `OnUnload`, `g_fnReloadConfiguration`, `VerifyRebuild.cpp`. Preserve categories/acronyms/real compounds and normal prose/path spelling. External APIs, public/lifecycle/serialized keys and compatibility filenames stay exact: `module.exports = { unload: Unload };`. No style alias for external Init/Alloc; public migration requires authorization.

### 8.6. Compile-Time Constants

`k` + PascalCase exclusively for named constexpr objects, including tables/static members, before use. Never runtime/non-constexpr const objects, macros, enum values, variables/parameters/template parameters, or functions (including constexpr/consteval).

### 8.7. Macros

Module-specific UPPER_SNAKE_CASE; uppercase arguments allowed. Thin facade over typed code; literal facades may use an immediately invoked lambda. Attribute/packing/warning/configuration helpers remain narrow and cleaned up (4.7).

### 8.8. Template Parameters and Type Aliases

Values: ordinary categories (`unLength`). Types: short PascalCase roles (`T` for one genuinely generic type; otherwise `TValue`, `TRecord`, `TAllocator`). Aliases: PascalCase; tiny internal traits may use STL-like lowercase only in narrow `Detail`. Broad public lowercase aliases require deliberate project-wide convention.

### 8.9. Acronyms, Enums, and State Names

Keep recognized acronyms uppercase (CPU/ABI/API/IO/HTTP/JSON/XML/TLS/SIMD/CRC/AES/RVA/PE): `GetCPUInformation`, `DumpRTTI`, `JSONReader`, `IsX64`. Preserve external `x86_64`, extensions, keys, flags.

Use enum class unless a documented boundary requires an unscoped enum (9.5). Domain states/instructions may use compact uppercase. Sequential enums initialize only first value to zero; external/sparse representations and bitmasks assign explicit values. Provide needed INVALID/UNKNOWN/NONE sentinels; never serialize convenience COUNT without a contract. Flags require typed operators, not implicit integer mixing.

### 8.10. Generated File and CLI Naming

- Deterministic role + stable index/key/range/schema/source discriminator, e.g. `TABLE_<INDEX>_<BEGIN>_<END>.cpp`; machine-owned roles may be uppercase, extensions retain toolchain spelling. Distinguish user files (`UserConfiguration.cpp`, `Hooks.asm`, `custom_rules.json`, `user/`).
- Document hex filename spelling/width; uppercase without `0x` recommended for sortable fields.
- Split artifacts by documented per-kind size/count/complexity caps and role-matching names (`--source-split-mb`, `--metadata-split-mb`, `--table-split-count`). Measure actual hard-cap payload where practical; label estimates.
- Remove stale outputs only within their generated family, never user files. Retry/recovery/post-processing changes affected outputs only; reuse safe caches/build trees, do not relog a full restart.
- Headers/READMEs/reports: stable key/value facts for addresses/sizes/versions/hashes/paths, explicit units/encodings.
- CLI order: input → output → generation/features → split/cache/performance → toolchain/platform → diagnostics/logging/dry-run/help. Pass safe parallel/incremental options through; no hidden expensive rebuild.
- Display CLI paths relative to cwd on the same filesystem root when safe; otherwise absolute. Internal operations may use absolute paths.

### 8.11. Generated-Code Contracts

Apply source rules unless a narrow generator exception is documented. Mark generated-only files; preserve user-editable content unless regeneration is authorized. Mixed files need stable begin/end regions; fragments declare standalone/included status.

Small typed documented extension points, compile-ready/copyable examples, minimal public headers, implementation files/Detail, thin macros. Emit in dependency order; validation: records → constants → assertions → helpers.

Transfer/package/restore scripts stage extraction in a temporary directory, reject absolute/traversal/escaping paths, read project ignore rules instead of duplicated lists, and preserve local/user configuration. Cross-artifact reference/relocation/import/label/metadata rewrites declare supported cases and clearly reject unsupported/out-of-range cases.

### 8.12. Namespace Names

PascalCase component/library roles, not generic grouping words; header implementation in `Detail`. No header file/namespace-scope `using namespace`. Closing comments: 19.4.

### 8.13. Directory Names

Lowercase responsibility names, not developers/temporary details. Illustrative: src/include/tests/tools/docs/examples/generated/user/custom/analysis/build/output; build/output normally ignored. Not a required repository layout.

### 8.14. Source File Names

One stable convention; prefer PascalCase C++ component pairs (`ProcessSuspender.h/.cpp`, `PEImage.h/.cpp`). Public header normally matches its primary component. Split by responsibility, not line count; generated names: 8.10.

### 8.15. Abbreviations

Complete words in all owned identifier roles/generated names; familiarity/existing use is no exemption. Ordinary replacements: init → initialize/initialization (verb/noun), deinit → deinitialize, reinit → reinitialize, alloc → allocate/allocation, dealloc → deallocate, config/cfg → configuration, info → information, disp → displacement, rel → relative (except standard relocation terms), addr → address, buf → buffer, ctx → context, idx → index, cnt → count, len → length, src → source, dst → destination.

Keep category prefixes, domain acronyms/terms (RVA/VA/TLS/PE/ELF/COFF/MASM/ASM/x86/x86_64/argv), external fields, public CLI flags. Operation spelling: 8.5.

## 9. Types, Declarations, and Qualifiers

### 9.1. Explicit Types

Signatures expose types/contracts; no unexplained deduction or deduced returns. Approved deduction: 11.4.

### 9.2. `const` Style

Simple scalars: west const (`const int nA`, `const float flB`, `const std::size_t unSize`). Compound declarations: const adjacent to its qualified type/pointer/reference (`char const* pszName`, `Block const& BlockValue`, `void const* const pAddress`). Never obscure constness, binding, ownership, or ABI; legacy changes: 2.3.

### 9.3. `const` on By-Value Parameters

Allowed for declarative implementation and visually consistent declarations/definitions. It qualifies the local copy, not function type/caller ownership; pointee/reference constness remains contractual.

### 9.4. `*` and `&` Binding

Bind to type: `void* pAddress`, `Page** ppPage`, `Block const& BlockValue`. Exactly one variable/object per C/C++ declaration; no combined scalar/array/pointer/reference declarations.

### 9.5. `typedef struct`, `typedef union`, and `typedef enum`

C/ABI/layout boundaries use `typedef struct _TAG { ... } TAG, *PTAG;` or matching union form. The reserved tag exception covers only these structure/union/unscoped-enum typedefs (2.5), not ordinary classes/scoped enums/other names. Pure C++ normally uses ordinary struct, enum class, or aliases.

Unscoped boundary enums require the tag, value/pointer aliases, and **explicit integral base selected for range, signedness, ABI/serialization**:

```cpp
typedef enum _SOME_MODE : unsigned char {
	SOME_MODE_DISABLED = 0,
	SOME_MODE_ENABLED
} SOME_MODE, *PSOME_MODE;
```

The base is illustrative, not a default: preserve/verify required size, alignment, values; no silent narrowing. Fixed-base syntax requires the documented C++/C toolchain contract, not arbitrary C-version compatibility. Value/bitfield union: 14.2.

### 9.6. `using` for Function Pointer Aliases

Prefer `using fnCallback = bool(*)(void* pData);`.

### 9.7. `noexcept`

Only when all operations cannot leak an exception, or all failures are caught/translated. Names/status returns do not prove it; allocation, formatting, containers, filesystem, callbacks/user code may throw. Incorrect noexcept terminates.

### 9.8. Default Arguments

Defaults belong in the declaration, not repeated in the definition. A first-and-only definition, including a required visible template, may own them. No redundant forward prototype just to relocate defaults (4.4).

### 9.9. Size and Index Types

Use std::size_t for sizes/capacities/indices unless ABI/format requires otherwise; fixed-width integers for contractual format/algorithm widths; unsigned names use `un`.

### 9.10. Fixed-Size Storage, Views, and Limits

Expose C++ extent via array references, std::array, templates, span/string_view. Raw pointer + adjacent size is valid at C/OS/hook/wire boundaries; validate both.

Non-trivial bounds need named constexpr constants; group related capacities in a Limits type when helpful. Raw arrays suit layout/C/stack/fixed-buffer contracts; std::array gives value semantics. Default-state constructor-free aggregates are value-initialized. Stored/returned views must not outlive storage.

### 9.11. `static_assert` for Compile-Time Contracts

Assert supported element sizes/capacities, configuration/features, and relied-on ABI/layout. Blank line before first assertion after constant/table/layout declarations; group related assertions.

### 9.12. Casts and Ignored Values

No C-style casts. Narrowest choice: static_cast (language/numeric/enum conversion), reinterpret_cast (reviewed address/representation boundary), const_cast (unavoidable justified cv change), std::bit_cast (same-size trivially copyable representation), dynamic_cast (deliberate RTTI/polymorphism).

Unneeded result: normal call, **no ignored-result cast**; handle correctness-relevant statuses and explain non-obvious safe omissions. **No maybe_unused or equivalent attribute/modifier.**

Check all supported configurations, uses, ABI/callback contracts, then:

1. Remove declarations unused everywhere, including functions/types/aliases/parameters/locals/globals/members/constants/macros/includes.
2. Remove unused private parameters and update callers.
3. Omit names for fixed ABI/callback positions unused everywhere.
4. Put configuration-local declarations in their using branch.
5. Only then narrowly suppress a required cross-configuration case.

Suppression must name the actual compiler diagnostic, explain the exact case, cover the smallest adjacent declaration/function, and restore prior state immediately. Never file/target/configuration/project-wide suppression or a global suppression macro. Prefer MSVC next-line `#pragma warning(suppress: ...)` if supported; otherwise narrow push/disable/pop or GCC/Clang equivalent. Never copy diagnostic IDs blindly.

### 9.13. Object Representation and Type Punning

A compiling cast does not permit unrelated-type access. Prefer std::bit_cast, std::memcpy, or unsigned char/std::byte representation access. Reinterpretation creates no lifetime, alignment, or valid aliasing; keep it at a reviewed boundary.

## 10. `class` and `struct` Organization

### 10.1. Repeated `public:` / `private:`

Repeated sections may group responsibilities. Labels align with the class; every member is one additional tab inside.

### 10.2. Order Inside a Class

Default: constructors/destructor → operations → accessors → private helpers → fields.

### 10.3. Member Initialization

**No in-class/default member initializers** in ordinary owned classes/structs. Every successful constructor establishes all members:

| Member requirement | Initialization |
| --- | --- |
| Ordinary default safely assignable after construction | Constructor-body assignment, one member per statement |
| Constructor-argument value | Initializer list |
| Required base, reference, const, value-initialized array, non-default-constructible member, or unsafe default-then-assign | Initializer list |
| Type already guarantees valid default construction | No redundant body assignment |
| ABI/serialized/mapped record | Preserve layout; initialize at use (10.4) |

Lists contain required/base/argument initialization; body assigns remaining ordinary defaults. Keep defaults in one place, lists in declaration order (shape: 5.1); raw scalars/pointers/handles/arrays have no automatic-state exemption.

Declare ordinary non-template runtime constructors in headers; define complete initialization in .cpp, including parameterized/delegating/copy/move constructors. Shortness is no inline exception.

A caller-visible template constructor in an ordinary runtime class may only forward through one initializer to the real non-template .cpp constructor: no allocation/validation/mutation/handle acquisition/platform setup duplication. Otherwise use a separate header-only factory/helper or correct source explicit instantiations.

Retain definitions genuinely required for caller-side dependent-template instantiation/constant evaluation, unless supported runtime instantiations can correctly be explicit. Do not add constexpr/templates/header-only status to evade placement. Defaults: 9.8; zero-argument defaulting: 10.5.

### 10.4. Layout and ABI Records

ABI/serialized/mapped/instruction/byte-copied records remain contractually simple: no added constructor/destructor/virtual behavior/base/hidden ownership unless permitted. Offsets alone do not preserve aggregate/trivial/implicit-lifetime properties.

Initialize at use (`SOME_RECORD Record {};`) or via helper. Assert relied-on standard-layout/trivially-copyable properties, sizeof/alignof/offsets (14.11).

### 10.5. Special Members

Use deletion/defaulting to express ownership. **Only zero-argument constructors may be = default**, including in-header, and only if every member becomes defined; never replace required scalar/pointer initialization. Other runtime constructors are source-defined or deleted, subject to 10.3's visibility exceptions.

Prefer rule of zero when members own correctly; rule of five for direct resource owners. Owners normally prohibit copies; moves require valid empty/destructible sources. Assignment operators may be defaulted when semantics permit.

### 10.6. Header-Only Value Wrappers

Small byte/name/token/buffer/compile-time wrappers may expose data()/size()/index/conversion helpers. Fixed storage, explicit size, no hidden allocation/ownership; constexpr/noexcept only when valid.

## 11. Function Body Style

### 11.1. Guard Clauses

Validate required invariants/inputs first; a private leaf may rely on caller-established preconditions (13.6).

### 11.2. Early Return

Handle failures immediately with guards, not deep nesting.

### 11.3. Phase-Based Structure

Preconditions → setup → main logic → cleanup → final return. Exact early-exit/main-return spacing: 6.2.

### 11.4. `auto`

| Situation | Required/default form |
| --- | --- |
| Every iterator/iterator-returning algorithm result | `auto itRecord = ...;` |
| Read-only / intentionally mutable / intentionally copied range element | `auto const&` / `auto&` / `auto` |
| Simple scalar, including known-contract scalar algorithm result | Explicit type; const scalars use west const |
| Complex pointer with suitable platform/project P* alias | Explicit alias, e.g. `PRecord pRecord` |
| Complex pointer without suitable alias | `auto*` + qualifiers, only if initializer preserves ownership/ABI clarity |
| Character/simple-scalar pointer or ordinary handle | Explicit type, including char const* const / wchar_t const* const |
| Local domain value from project function/factory, role clear | `auto`, e.g. `auto ModuleFileName = GetDebugModuleName(...);` (optional-string wrapper) |
| Named local lambda / usefully unnameable generated-dependent facade result | `auto` |
| Ownership-bearing allocation/smart pointer / ABI-system record | Explicit owner/record type |

Default ranges to read-only reference; mutation/copy intentional. Names still follow 8: `RegistryEntry` (record), `strName` (string), `Predicate` (lambda). Never derive iterator/range spellings via decltype/value_type/remove_reference_t. Avoid structured bindings hiding important fields/types.

decltype: only genuinely expression-dependent types not clearly/correctly expressible by explicit types or approved auto. decltype(auto): only forwarding/adapters needing exact value/reference preservation, never locals/iterators/ranges/convenience returns.

A narrow literal facade may need `decltype(S)` for extent and `decltype(S[0])` for narrow/wide element type, e.g. `std::extent_v<std::remove_reference_t<decltype(S)>>`; its dependent constexpr auto and deduced return are allowed, not general shortcuts.

Initializer semantics are independent (7.9): `std::unique_ptr<char[]> pBuffer(new (std::nothrow) char[unSize]);` requires direct construction because the raw-pointer constructor is explicit.

### 11.5. Explicit `return true;` / `return false;`

Explicit documented result on every normal value-returning path. Fixed boolean outcomes use true/false; direct predicates need no redundant branches. No value returns in constructors/destructors/void functions; preserve JavaScript result contracts.

### 11.6. Cleanup and Ownership Near the Failure Point

Attach resources to RAII immediately, not after several raw acquisitions. Required manual boundary cleanup stays visible near failures on every exit. Query-like helpers must not hide allocation/locking/transfer/cleanup.

Single-responsibility `goto Cleanup` is allowed when flatter in C-compatible/multi-resource low-level code; never bypass non-trivial initialization.

### 11.7. Local Lambdas as Phase Helpers

Narrow local parsers/validators/emitters/adapters may clarify phases. PascalCase object names without fn (8.1), close to use, obvious narrow captures, no concealed resource effects. Extract independent responsibilities to private helpers; do not duplicate lambdas.

### 11.8. `constexpr` and `consteval` Bodies

Deterministic readable algorithms; loops/mutable locals/fixed arrays/helper records/explicit casts allowed. Prefer fixed storage, assert limits, return explicit multistage results, avoid compile-time exceptions, exclude I/O/logging/platform calls.

## 12. Error Handling

### 12.1. Main Model

Prefer explicit bool/null/status-enum/error/result reporting. No attributes solely to force inspection; expose handling through types/names/checks/review/tests.

### 12.2. Exceptions

Only in a consistent subsystem with a permitting boundary; prefer explicit status for systems/low-level/compile-time/real-time/ABI/callback/hook code. Never escape C ABI, OS callback, thread entry, destructor, noexcept, or injected/hooked caller that does not expect exceptions. Catch/translate at the boundary with diagnostic context.

### 12.3. Platform Error State

Capture GetLastError/errno/library transient error immediately after failure, before cleanup/logging/formatting/allocation/another API can overwrite it.

### 12.4. SEH and Platform-Specific Error Handling

Only for an actual platform need; minimize the protected region. Not ordinary C++ control flow or proof of memory safety.

### 12.5. Diagnostic Ownership

Low-level helpers usually return detailed status; context-owning layer logs. Propagate context, not duplicate logs at every layer. Include operation and relevant non-secret path/offset/index/status/expected size; never keys/passwords/tokens/plaintext secrets/full sensitive buffers.

### 12.6. Compile-Time Error Reporting

Prefer result with success, stage, line/index/offset, output, stable error code/message. Carriers constructed in constant evaluation must support it; a non-constexpr runtime constructor is not a compile-time example.

## 13. API Design

### 13.1. Explicit API Families

Semantic families: FindByName/FindByAddress/FindByIndex. Overload for natural representations with identical semantics; distinct names for different ownership/blocking/allocation/lookup/encoding/failure.

### 13.2. ANSI/Wide and Generic Wrapper

A/W + generic wrapper allowed for matching platform domains; selection predictable, never silently lossy.

### 13.3. Callback APIs

Expose type, e.g. `using fnEnumerationCallback = bool(*)(void* pEntry, void* pUserData);`. Prefer explicit user data at ABI boundaries. Document lifetime/thread/lock/reentrancy/false-stops-enumeration. No user/untrusted callbacks under internal locks unless required/documented.

### 13.4. Getters

Raw getters are valid for raw concepts. No concealed allocation/blocking/observable mutation/ownership transfer; name such behavior explicitly.

### 13.5. Ownership and Lifetime

Make ownership/lifetime clear in types/names/adjacent contracts. Raw pointers borrow by default; `std::unique_ptr<T>` owns one, `std::unique_ptr<T[]>` an array, move transfers ownership; span/string_view borrow. Document handle closer, returned view/address source lifetime, output capacity/written length. Owner/Borrowed/Out roles only when useful.

### 13.6. Nullability and Output Parameters

State optional-null meaning. Validate required pointers immediately except caller-proven private leaves. Outputs use Out suffix and commit only after validation/success; prefer named results for clearer multi-value/partial-state reporting.

### 13.7. Pointer and Size Pairs

Keep pointer next to size/capacity. Validate null, zero-size policy, entire range, overlap before use. Never infer binary size from a terminator unless the API accepts a string.

### 13.8. ABI and Versioning

Preserve calling conventions/packing/widths/symbols/ownership. No cross-binary STL/exceptions/compiler RTTI/allocator ownership without deliberate shared-toolchain/runtime contract. Extensible records require size/version fields and clear unsupported-value rejection.

## 14. Low-Level and Platform-Aware Code

Only where ABI/layout, mappings/protection, intrinsics, machine code, system APIs, binary formats, or compile-time bytes require it.

### 14.1. Allowed Tools

Reviewed casts/bit_cast/offsetof/sizeof/alignof, unions/bitfields, raw arithmetic, intrinsics, bytes/fixed buffers, architecture branches, macros, constexpr tables are valid tools, never exemptions from bounds/lifetime/aliasing/alignment/error checks.

### 14.2. Layout-First Structures

Layout accuracy wins. Complete-value/bitfield layout uses the boundary typedef with an anonymous struct and directly exposed fields; one blank line after the complete value:

```cpp
typedef union _FLAGS {
	unsigned int m_unValue;

	struct {
		unsigned int m_unA : 1;
		unsigned int m_unB : 1;
	};
} FLAGS, *PFLAGS;
```

No style-only separate FlagBits/named m_Bits replacement. Preserve public field access and ABI. Document/assert widths/offsets/packing/endianness/architecture.

Bitfield allocation is implementation-defined; anonymous struct requires documented compiler-profile extension support. Diagnostics use only 9.12's narrow justified suppression; unsupported targets need an explicit representation decision, not silent substitution. Use bitfields only under controlled compiler/ABI contracts, otherwise masks. This layout does not permit portable inactive-union-member punning (9.13).

### 14.3. Address and Range Validation

Validate complete ranges before dereference/copy, not just starts; prefer overflow-safe subtraction:

```cpp
if ((unOffset > unBufferSize) || (unDataSize > (unBufferSize - unOffset))) {
	return false;
}
```

Before following mapped/binary data, validate every header/directory/table/count/multiplication/nested range against a trusted outer size.

### 14.4. Macros

Only for actual directives, flags/tables, repetitive low-level patterns, compile-time results, or preprocessor-only information. Prefer typed functions/templates/constants if they preserve required information (4.7).

### 14.5. Architecture Branches

Explicit branches for differing ABI/layout; reject unsupported targets. Detect compiler separately from architecture; pointer width identifies neither instruction set nor ABI.

### 14.6. Byte, Endian, and Bit-Level Helpers

Localize conversion/byte aliasing. Raw storage: `unsigned char`/`std::uint8_t`/`std::byte`. Names expose order (ReadU32LE/WriteU64BE). Support only implemented widths; assert compile-time widths, explicitly cast shifts/masks/sign extension. Every concrete fixed-width reader checks its own bounds and constructs the result explicitly.

### 14.7. Compile-Time Tables and Algorithms

Explicitly typed constexpr tables with k names, before consumers, grouped with algorithms. Assert/range-check counts/indices; avoid mutable global tables unless mutation is required.

### 14.8. Compile-Time Obfuscation, Hashing, and Crypto Utilities

Only for real project needs: typed namespaced implementation, narrow facade, deterministic compile-time work, explicit seed/salt/timestamp/line/counter mixing. Build variation is not randomness; obfuscation is no cryptographic boundary, embedded literals/keys remain recoverable. Wipe temporary plaintext where practical; reviewed cryptography for real confidentiality/integrity.

### 14.9. Integer Overflow and Narrowing

Validate addition/multiplication/alignment/count-to-bytes/displacements **before** overflow/narrowing, in the source type. Multiplication rejects zero element size or `unCount > (std::numeric_limits<std::size_t>::max() / unElementSize)` before computing. Branches/RVAs/offsets/differences use wide signed/unsigned intermediates, then target-encoding range checks. Never validate truncated results.

### 14.10. Alignment, Lifetime, and Aliasing

Ensure alignment/live object before typed access. Copy packed/unaligned input to aligned storage or read bytes unless an explicitly supported platform contract permits unaligned access. Casts create no lifetime; use language/storage-permitted mechanisms. Confine address arithmetic to reviewed std::uintptr_t/byte boundaries; validate before final typed conversion/dereference.

### 14.11. Packing and ABI Checks

Narrow `#pragma pack(push, N)`/`#pragma pack(pop)` scope; no leakage into unrelated declarations/includes. Assert relevant sizeof/alignof/offsetof for each relied-on external record on every supported architecture. One architecture compiling proves nothing about another's layout.

### 14.12. CPU Features and Intrinsics

Compile-time architecture does not prove runtime instruction support. Detect CPU/OS and required extended-state support for optional instruction families; isolate feature code, provide fallback/clear rejection, test each enabled architecture/configuration.

### 14.13. `volatile`, Atomics, and MMIO

Volatile neither synchronizes nor legalizes races; use atomics/locks/platform synchronization. Reserve it for actual observable-access contracts (MMIO/compiler primitives); isolate MMIO wrappers and preserve barriers/order.

## 15. Resource Management

### 15.1. RAII as the Default

Immediately attach handles/mappings/memory/locks/descriptors/sockets/temporary state to small owners. Destructors never throw; cleanup tolerates empty/moved-from state. Constructors/special members: 10.

### 15.2. Manual Lifecycle

Initialize/Release allowed for domain-required explicit/recoverable setup. Expose state, release partial initialization safely, make repeated release harmless where practical, restore empty state, prevent pre-initialize/post-release use. Never mix RAII and undocumented manual ownership of one resource.

### 15.3. Owning Pointers and Handles

Prefer `std::unique_ptr` for single ownership; raw pointers borrow unless an external contract says otherwise. Wrap ABI/OS/shared-memory/binary resources at C++ boundaries, not smart pointers inside records. `std::shared_ptr` requires real shared lifetime, not avoidance of owner design.

### 15.4. Move Semantics

Transfer exactly one resource and clear source. Assignment handles self-move and releases old destination before acquisition. Moved-from state stays destructible; other operations require a documented empty-state contract.

### 15.5. Temporary Plaintext and Sensitive Buffers

Prefer short-lived small owners: no copies, moves only with source clearing, destructor wiping through approved secure-zero primitives (SecureZeroMemory/explicit_bzero/memset_s where available). Localize/test wiping against optimization removal; never log contents. Global/static decrypted buffers/runtime keys need explicit threat-model acceptance.

## 16. Synchronization

### 16.1. Explicit Primitives

Use appropriate mutex/shared-mutex/critical-section/SRWLOCK/atomic/platform primitives, never volatile/sleep loops/incidental calls.

### 16.2. Local Visibility of Lock Ownership

Visible local acquisition/release, role-named RAII guard, smallest correct critical section; never split one invariant merely for brevity.

### 16.3. Lock Ordering

Document stable multi-lock order near declarations/contracts; no caller-dependent order. Use `std::scoped_lock`/platform equivalent for a known set.

### 16.4. Callbacks, Blocking, and Reentrancy

No callbacks/virtual extensions/logging sinks/IPC/filesystem/network/blocking operations under internal locks unless required; document reentrancy/deadlock constraints at that call site.

### 16.5. Atomics and Memory Order

Use atomics only for correctly expressible invariants; groups usually need locks. Default sequential consistency; weaker orders need a documented concurrency proof, never performance alone.

### 16.6. Condition Variables and Waiting

Predicate waits handle spurious wakeups; monotonic clocks for timeouts/elapsed duration, never jumping wall time.

### 16.7. Thread-Safety Contracts

Public APIs state confinement, lack of safety, concurrent-reader safety, or internal/external synchronization, plus blocking operations/callback threads.

## 17. STL Profile

### 17.1. Preferred Types

Use standard containers/strings/array/span/string_view/optional/unique_ptr/filesystem paths/error_code where ownership/value semantics/non-throwing errors become clearer, never in incompatible ABI/binary/shared-memory/platform/fixed-buffer contracts.

### 17.2. Views and Lifetime

Non-owning views must not reference expired temporaries/stack storage, reallocating containers/strings, or asynchronously destroyed owners (9.10).

### 17.3. Explicit Loops

Prefer loops when more auditable; algorithms allowed when clearer without hidden control/error paths. Iterators/ranges: 11.4.

### 17.4. Container Access

operator[] requires locally validated indices; at() only where exception-based bounds reporting fits. Never retain pointers/references/iterators/views across invalidating operations. Reserve useful reliable upper bounds, not speculative enormous capacity.

### 17.5. Higher-Level Wrappers

Clarify ownership/lifetime without hiding byte order, offsets, ABI widths, lock state, handles, or other domain-critical facts.

## 18. Compile-Time and Header-Only Utilities

### 18.1. General Model

Typed implementation before optional macro syntax: 4.6's dependency order and 10.3's constructor/visibility exceptions apply; header-only status never bypasses them.

### 18.2. Compile-Time Tables

Follow 14.7; large tables also need a generation source/documented derivation and count/element-width assertions.

### 18.3. Byte Serialization Helpers

Explicit widths/serialization (14.6); no host endianness/padding dependence except intentional host-representation algorithms.

### 18.4. Public Compile-Time API Macros

Thin typed-code facades allowed for otherwise unavailable extent, `__LINE__`/`__COUNTER__`, architecture selection, or expression-like lazy construction; narrow immediately invoked lambdas allowed. Required literal-dependent decltype/constexpr auto/deduced return: 11.4, not general deduction exemptions.

### 18.5. `constexpr` / `consteval` Entry Points

`consteval` for mandatory compile-time execution; `constexpr` for compile-time/runtime use. Readable loops/fixed storage, explicit multistage results instead of template-substitution noise (11.8/12.6).

### 18.6. Compile-Time Resource Limits

Name applicable input/instruction/recursion/output/diagnostic limits; static_assert messages identify exceeded contracts. No unexplained recursion/constant-evaluation compiler hangs.

## 19. Comments

### 19.1. Banners

Stable responsibilities, not every helper; exact widths: 4.5.

### 19.2. Phase Comments

Explain intent/invariant/reason/contract/non-obvious consequence, not obvious statements. Short phase labels may help long functions.

### 19.3. Layout and Safety Comments

Document offsets/sizes, encoding/endian, packing/alignment, ownership/lifetime, architecture, unusual casts/barriers/cleanup reasons; never replace checks/assertions.

### 19.4. Namespace Closing Comments

Every named namespace: `} // namespace Name`; anonymous: `} // namespace`.

### 19.5. TODO and FIXME

Actionable/searchable missing behavior/failure, not just a person; issue ID when available; remove stale notes.

### 19.6. Public Documentation

State safe-use ownership/nullability/threading/encoding/units/errors/lifetime not evident in signatures; do not repeat names. See 25.4.

## 20. What This Style Dislikes

Reject hidden behavior/ownership, needless abstraction, deep nesting, unchecked assumptions, indiscriminate deduction, leaking macros, fake synchronization, and unsupported security claims. Apply detailed rules 4–19, not a duplicate checklist.

## 21. Canonical Templates

The following construction recipes combine the rules referenced within this document. They describe local shapes, not standalone programs unless complete (2.1).

### 21.1. Canonical Function

Validate → acquire/attach owner → operate → commit → separated final return. Guards: 6.2; cleanup: 11.6/15.1. Early terminal actions remain adjacent to returns.

### 21.2. Canonical Class

Lifecycle → operations → accessors → helpers → fields (10.1–10.2). Header declares runtime constructors, .cpp defines them. Defaults/body vs argument/required initializer lists: 5.1/10.3. Defined state for all members, no in-class initializers; ownership/special members: 10.5.

### 21.3. Canonical Overload Family

FindByName/FindByAddress/FindByIndex; explicit semantic distinctions and natural-representation overloads follow 13.1.

### 21.4. Canonical Low-Level Record

Boundary typedefs (9.5), canonical anonymous bitfield union (14.2), use-site value initialization (10.4), relied-on standard-layout/triviality/size/alignment/offset assertions (14.11).

### 21.5. Canonical Header-Only Compile-Time Utility

Guard/configuration (4.1/4.3) → typed dependency order/facade (4.6/18), constexpr tables before consumers, limits/assertions, namespace closing comment, immediate temporary-macro cleanup. Preserve constructor visibility (10.3) and deduction constraints (11.4).

## 22. Rules in MUST / SHOULD / MAY Form

Precedence/normative meanings: 2.2. Detailed rules and their exceptions are authoritative; these categories add no second checklist.

### MUST

Apply every relevant required rule, including generated source/examples and verification; respect ownership exclusions.

### SHOULD

Follow documented defaults; deviations require the local reason in 2.2.

### MAY

Use permitted options only within their language/domain/contract constraints; do not introduce every allowed tool.

## 23. One-Line Summary

Make code readable top-to-bottom, with visible ownership/dependencies and names/constants/macros/buffers/control flow that mean exactly what they say.

## 24. Non-Goals

Not a replacement for language/ABI/platform/security/cryptographic specifications; no external-contract override, unrelated restyling, universal low-level tooling, needless abstraction, secret-management claim for obfuscation, or correctness proof from formatting/naming alone.

## 25. Portable Maintenance and Verification Rules

### 25.1. Behavior-Preserving Cleanup

After supported-path checks, remove unused private parameters/call arguments/locals, dead/unreachable code, stale comments. Remove includes only after checking every dependent target. Merge temporaries only if auditable; extract stable responsibilities/meaningful duplication. Preserve public/callback/ABI/serialization/generated/CLI contracts; separate behavior/style where practical (2.3).

### 25.2. Generated Artifact Ownership

Apply 2.5/8.10–8.11 to generated source/scripts/configuration/examples. Mark machine-owned outputs, preserve user regions/local configuration, keep stale cleanup within its generated family. Regeneration/recovery/extraction never silently overwrites or escapes into user-owned paths.

### 25.3. Logging and Terminal Output

Complete redirect-safe records; only interactive rendering may clip/rewrite live lines. No redirected spinners/control sequences/truncation. Distinguish stage from total elapsed time.

#### Severity and Message Ownership

Canonical severities: [DEBUG] debug detail; [INFO] ordinary state/successful progress; [WARNING] recoverable/degraded/fallback/retry; [ERROR] failed/unrecoverable operation.

Typed thread-safe logger owns one bracketed severity; optional timestamp/process/thread metadata may precede it. LogDebug/LogInfo/LogWarning/LogError messages start with operation/context, never another severity prefix. Never detect/strip one at runtime.

```cpp
LogError("OpenResource failed Error=0x%08X", unError);
```

Terminal/file copies of an event preserve the same operation/result/IDs/paths/sizes/status/error, differing only in presentation metadata. Capture transient errors first (12.3). Debug diagnostics compile/emit only in debug; conditional debug terminal output also uses the typed file logger when enabled. Macros may capture file/line. Never log secrets/full sensitive buffers.

#### Diagnostic Fields

Operation first, stable fields: `Error=0x%08X`, `Status=0x%08X`, `HRESULT=0x%08X` for contractual hex widths; `Result=%d` or semantic signed-result name; applicable `Process=%lu`, `Thread=%lu`, `Address=%p`, `Size=%zu`, `Path="..."`.

No alternate case/spacing/spelling/base within a field family (`Error =`, `error=`, `ErrorCode=`). Capture once immediately after failure, before any cleanup/formatting/logging/allocation/output; both sinks use that same value.

#### Target-Scoped Log Files

Use canonical target process/file/workspace/device identity, not launcher/cwd. Resolve once; derive collision-resistant basename and document stable naming/architecture/configuration/session qualifiers. No silent wrong-target/ambiguous fallback/relocation: report attempted/final paths, unresolved identity, or unwritable destination through available terminal diagnostics.

#### CLI Help and Result Markers

Validate the entire CLI before execution/result output. No args, unknown command, missing/extra operands, invalid flags: version/help/usage + documented parser exit code, **no operation-result marker**.

A valid executed state-changing operation prints exactly one documented success/failure marker as the final terminal line, not a severity. It complements failure diagnostics and agrees with exit status. Queries/reports omit it; relaunch/elevation wrappers propagate child status without duplicating it.

### 25.4. Markdown Documentation

Explain where/what/when/why/safe use, related files, limitations, common mistakes. Space-indented prose; language-styled fences. Large docs need AI navigation + contents; never invent unsupported architecture.

Align docs/examples/packages/templates/test doubles with current exports, parameter order/meaning, results/sentinels, lifecycle, branch/platform availability. Migration is incomplete while copyable material teaches old contracts. Synopsis/table/callback/example parameters use actual categories and consistent Systems Hungarian + PascalCase (2.6/8.1). Preserve exact external declarations/exports/property/serialized keys; placeholders never rename contracts.

### 25.5. AI-Agent Editing Rules

1. **Read the entire current guide before project analysis/edits.** After compaction/reset/recovery/context loss, reread fully, then reconstruct actual files/status/diff/progress; never resume from remembered rules or presumed coverage. User guide updates require another complete read before code work.
2. **One bounded chunk of one file at a time.** Read only it; no read-ahead, parallel chunks, or batch review. Inspect only necessary related definitions/usages, then return. Keep the chunk, applicable rules, concise progress in working context.
3. Understand logic/dependencies → check every applicable rule → correct → preserve behavior/contracts/consistency → verify no unrelated edits → mark complete → only then advance. Prefer direct structural edits, not appended normalization passes.
4. Preserve unrelated dirty work/user regions. No skipping small helpers/boilerplate/templates/generated-looking files; classify each current path (2.5). Huge/generated trees still use bounded chunks.
5. Track completed files/chunks, current chunk, remaining paths, recurring fixes, checks. Temporary tracking/scratch files stay out of final diff and are removed before completion. Do not repeat answered questions.
6. Update newly agreed recurring guide rules and behavior/generated-contract tests/docs. Focused checks first, broaden for affected surface; report verified/unverified facts accurately.

**Full-project completion gate:** finish the main pass, then full chunk-by-chunk verification passes. Any verification-pass or later check-driven code/template/documentation fix restarts a full pass from the beginning. Complete only after a **zero-change full pass**, relevant checks, final diff review, and temporary-file removal. Searches/green builds never prove coverage.

### 25.6. Builds, Warnings, and Tests

Predictable test/fixture paths; one clearly named scenario per module/script; supported configuration/architecture parameters. Compile edited targets at strict project warning level; new warnings fail. Unused declarations: 9.12 only.

Test affected debug/release differences and all changed supported architecture/ABI paths; compatible ASan/UBSan/iterator/platform diagnostics. Cover parser/range/overflow/allocation/malformed-input boundaries/failures and deterministic regeneration/safe stale cleanup. Focused checks precede shared-infrastructure/public-API regressions.

### 25.7. Mechanical Enforcement

Use the actual documented **read-only** repository audit, owned files only by default (2.5). Diagnostic inclusion grants no write authority. Without a checker, document available formatter/linter/manual checks, never invent a command. Changed-only may begin (working tree/index/untracked); final verification runs the default full audit.

Maintain appropriate editorconfig, aligned formatter/linter/style audit, ABI assertions, CI formatting/tests/build/architecture matrices. No trailing whitespace, accidental repeated blank lines, or missing final newline.

Check **every applicable rule**, not just regex matches:

| Audit family | Sections |
| --- | --- |
| Tabs/blank lines/braces/banners/signatures/calls/grouping/ternaries | 4.5, 5–7 |
| Includes/labels/macros/boundaries/collisions/linkage/dependencies | 4 |
| Categories/recursive primitive pointers/full words/operations/acronyms/k; JS imports and typed doc parameters | 2.6, 8, 25.4 |
| One declaration/qualifiers/initialization/literals/direct std calls; casts/unused/suppression/constructors/auto/decltype | 7, 9–11 |
| Tagged boundary typedefs/enum base/anonymous bitfields; ABI/ranges/lifetime/errors/concurrency | 9.5, 10.4, 12–18 |
| Logging fidelity/severity/CLI markers/docs/examples | 19, 25.3–25.4 |
| Ownership/profile/generated-user preservation/scoped diff/configurations/chunk coverage/zero-change pass | 2, 8.10–8.11, 25.1–25.6 |
| Guide consistency | Positive examples obey rules; negative examples labeled; links resolve; summaries preserve detailed rules |

Read-only candidate searches (restrict to owned files; review matches):

```text
git diff --check
rg -n "\[\[maybe_unused\]\]|static_cast<void>\s*\(" -g "*.{c,cc,cpp,cxx,h,hh,hpp,hxx}" .
rg -n "decltype\s*\([^)]*\.(begin|end)\s*\(" -g "*.{c,cc,cpp,cxx,h,hh,hpp,hxx}" .
rg -n "^// [=-]+$" -g "*.{c,cc,cpp,cxx,h,hh,hpp,hxx}" .
rg -n "^// (={64}|-{64})$" -g "*.{c,cc,cpp,cxx,h,hh,hpp,hxx}" .
```

Compare all banners with correctly sized candidates, not only the latter. Strings/comments/negative examples/generated/external/ABI/compatibility contracts can match; regex is not semantic proof. Review decltype under 11.4; use parsers/compiler diagnostics for casts/declarations/unused/type-sensitive rules.

Final sequence: focused searches → diff check + full relevant diff review → guide-aligned formatter check-only → affected debug/release/architecture warning-enabled builds → focused/broader tests → required full verification pass after fixes. Investigate in-scope failures, rerun relevant checks, report unavailable checks and reasons. Formatter output never overrides guide rules or proves ownership/errors/ranges/concurrency/ABI/diagnostics/generated semantics.
