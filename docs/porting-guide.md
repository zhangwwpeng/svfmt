# Porting guide

How to close a gap with `verible-verilog-format`.

## Workflow

1. Find a difference:
   ```sh
   python3 tests/run_verible_cases.py build/svfmt tests/verible_cases.jsonl --out build/cases_report
   python3 tools/show_cases.py build/cases_report/diff.txt formatter_module 5
   ```
2. Compare the trees and partitions:
   ```sh
   verible-verilog-syntax --printtree x.sv               # Verible CST
   build/svfmt --dump_cst x.sv                            # svfmt CST
   verible-verilog-format --show_token_partition_tree x.sv
   build/svfmt --show_token_partition_tree x.sv
   ```
3. Find the Verible case that handles the construct, in a reference checkout
   of `verible/verilog/formatting/{tree-unwrapper,token-annotator,align}.cc`.
   Map its `NodeEnum` to our tag with the table below, and port the case.
4. Re-run `ctest`. Raise `--min-same` in `CMakeLists.txt` when the pass count
   goes up.

If slang's tree lacks a grouping node that Verible relies on, add a
`GroupKind` and a `ConvertSpecial()` case in the bridge. That is usually
easier than reproducing the grouping in the unwrapper.

## Node mapping (Verible → svfmt tag)

| Verible `NodeEnum` | svfmt |
| --- | --- |
| `kDescriptionList` | `ListKind::CompilationUnit_members` |
| `kModuleDeclaration` / `kInterfaceDeclaration` / `kPackageDeclaration` | `ModuleDeclaration` / `InterfaceDeclaration` / `PackageDeclaration` |
| `kModuleHeader` | `ModuleHeader`, `InterfaceHeader`, `ProgramHeader`, `PackageHeader` |
| `kModuleItemList`, `kPackageItemList` | `ListKind::ModuleDeclaration_members` |
| `kGenerateItemList` | `ListKind::GenerateBlock_members`, `GenerateRegion_members` |
| `kFormalParameterList` | `ListKind::ParameterPortList_declarations` |
| `kParamDeclaration` | `ParameterDeclaration`, `TypeParameterDeclaration`, `ParameterDeclarationStatement` |
| `kPortDeclarationList` | `ListKind::AnsiPortList_ports` |
| `kPortList` | `ListKind::NonAnsiPortList_ports`, `FunctionPortList_ports` |
| `kPortDeclaration` | `ImplicitAnsiPort` |
| `kPort` | `ImplicitNonAnsiPort`, `ExplicitNonAnsiPort`, `ExplicitAnsiPort` |
| `kDataDeclaration` (variables) | `DataDeclaration` (+ `ClassPropertyDeclaration`) |
| `kDataDeclaration` (instances) | `HierarchyInstantiation` |
| `kInstantiationType` | `GroupKind::kInstantiationType` |
| `kGateInstanceRegisterVariableList` | `ListKind::DataDeclaration_declarators`, `HierarchyInstantiation_instances` |
| `kRegisterVariable` | `Declarator` (in a declarator list) |
| `kGateInstance` | `HierarchicalInstance` |
| `kActualParameterByNameList` / `kParamByName` | `ListKind::ParameterValueAssignment_parameters` / `NamedParamAssignment` |
| `kPortActualList` / `kActualNamedPort` / `kActualPositionalPort` | `ListKind::HierarchicalInstance_connections` / `NamedPortConnection` / `OrderedPortConnection` |
| `kNetDeclaration` | `NetDeclaration` |
| `kContinuousAssignmentStatement` / `kNetVariableAssignment` | `ContinuousAssign` / `AssignmentExpression` |
| `kAlwaysStatement`, `kInitialStatement`, `kFinalStatement` | `AlwaysBlock`, `AlwaysCombBlock`, `AlwaysFFBlock`, `AlwaysLatchBlock`, `InitialBlock`, `FinalBlock` |
| `kSeqBlock` / `kParBlock` / `kGenerateBlock` | `SequentialBlockStatement` / `ParallelBlockStatement` / `GenerateBlock` |
| `kBegin`, `kEnd` | `GroupKind::kBegin`, `kEnd` |
| `kBlockItemStatementList` | `ListKind::BlockStatement_items` |
| `kFunctionItemList`, `kStatementList` (tf bodies) | `ListKind::FunctionDeclaration_items` |
| `kConditionalStatement` / `kConditionalGenerateConstruct` | `ConditionalStatement` / `IfGenerate` |
| `kIfClause`, `kIfHeader`, `kIfBody`, `kElseBody` | `GroupKind::*` with the same names |
| `kElseClause` | `ElseClause` |
| `kCaseStatement` / `kCaseItemList` / `kCaseItem` / `kDefaultItem` | `CaseStatement` / `ListKind::CaseStatement_items` / `StandardCaseItem` / `DefaultCaseItem` |
| `kForLoopStatement`, `kForeachLoopStatement`, `kWhileLoopStatement`, `kRepeatLoopStatement` | `ForLoopStatement`, `ForeachLoopStatement`, `LoopStatement` |
| `kLoopHeader` | `GroupKind::kLoopHeader` |
| `kProceduralTimingControlStatement` | `TimingControlStatement` |
| `kNonblockingAssignmentStatement`, `kBlockingAssignmentStatement` | `ExpressionStatement` holding `NonblockingAssignmentExpression` / `AssignmentExpression` |
| `kFunctionDeclaration`, `kTaskDeclaration` | `FunctionDeclaration`, `TaskDeclaration` |
| `kFunctionHeader`, `kTaskHeader` | `GroupKind::kSubroutineHeader` |
| `kClassDeclaration` / `kClassHeader` / `kClassItems` | `ClassDeclaration` / `GroupKind::kClassHeader` / `ListKind::ClassDeclaration_items` |
| `kFunctionCall`, `kSystemTFCall` | `InvocationExpression` |
| `kParenGroup` (call arguments) / `kArgumentList` | `ArgumentList` / `ListKind::ArgumentList_parameters` |
| `kMacroCall` / `kMacroArgList` | `MacroUsage` / `GroupKind::kMacroArgList` |
| `kPreprocessorDefine` | `DefineDirective` |
| `kEnumNameList` / `kEnumName` | `ListKind::EnumType_members` / `Declarator` |
| `kStructUnionMemberList` / `kStructUnionMember` | `ListKind::StructUnionType_members` / `StructUnionMember` |
| `kDistributionItemList` / `kDistributionItem` | `ListKind::DistConstraintList_items` / `DistItem` |

Constructs not yet mapped include UDPs (`UdpDeclaration`), specify blocks,
clocking blocks, covergroup details, assertion bodies, and `randomize ... with`.
See the roadmap.
