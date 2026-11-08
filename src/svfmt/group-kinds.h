// Synthetic grouping nodes that the slang bridge inserts so that the CST has
// the shape Verible's formatter expects in places where slang's tree is
// flatter (if header/body, begin/end with labels, loop headers, ...).

#ifndef SVFMT_GROUP_KINDS_H_
#define SVFMT_GROUP_KINDS_H_

#include <string_view>

#include "slang/syntax/SyntaxKind.h"

namespace svfmt {

enum class GroupKind : int {
  kIfClause = 30000,   // [if-header, if-body]
  kIfHeader,           // unique? if ( predicate )
  kIfBody,             // statement / generate item after if (...)
  kElseBody,           // statement / generate item after else
  kBegin,              // label? begin/fork : name?
  kEnd,                // end/join : name?
  kLoopHeader,         // for/foreach/repeat/while ( ... )
  kSubroutineHeader,   // function/task prototype ;
  kClassHeader,        // virtual? class name #(...) extends ... ;
  kInstantiationType,  // data type (+ parameter values) of a declaration
  kParenGroup,         // ( ... ) of a macro call
  kMacroArgList,       // macro call arguments and separators
  kCovergroupHeader,   // covergroup name (ports) @event ;
  kAssertionDeclBody,  // variables and body of a property/sequence declaration
  kLast_ = kAssertionDeclBody,
};

inline constexpr slang::syntax::SyntaxKind G(GroupKind g) {
  return static_cast<slang::syntax::SyntaxKind>(g);
}

inline bool IsGroupTag(int tag) {
  return tag >= static_cast<int>(GroupKind::kIfClause) &&
         tag <= static_cast<int>(GroupKind::kLast_);
}

inline std::string_view GroupKindName(int tag) {
  static constexpr std::string_view kNames[] = {
      "kIfClause",          "kIfHeader",   "kIfBody",       "kElseBody",
      "kBegin",             "kEnd",        "kLoopHeader",   "kSubroutineHeader",
      "kClassHeader",       "kInstantiationType", "kParenGroup", "kMacroArgList", "kCovergroupHeader",
      "kAssertionDeclBody",
  };
  return IsGroupTag(tag) ? kNames[tag - static_cast<int>(GroupKind::kIfClause)]
                         : std::string_view();
}

}  // namespace svfmt

#endif  // SVFMT_GROUP_KINDS_H_
