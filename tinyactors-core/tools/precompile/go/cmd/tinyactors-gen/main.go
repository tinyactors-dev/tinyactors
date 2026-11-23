// tinyactors-gen writes the Go code of a chart whose datamodel is "go" (a
// spike: tools/precompile). Run it with go generate beside the chart:
//
//	//go:generate go run tinyactors.dev/precompile/cmd/tinyactors-gen order.scxml
//
// It asks core's compiler for the chart's expression table
// (ta-precompile-table), type-checks every expression in the package, as Go,
// inside a function whose locals are the chart's variables, and writes
// <chart>_gen.go: a struct whose fields are the variables (typed by their
// <data> expressions) and a switch per kind of expression, each case the
// expression with its variables read from the struct. A type error is
// reported at the chart's line; //line directives keep the chart's lines in
// the compiler's errors and in panics.
package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"go/ast"
	"go/format"
	"go/parser"
	"go/printer"
	"go/token"
	"go/types"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	"golang.org/x/tools/go/ast/astutil"
	"golang.org/x/tools/go/packages"
	"golang.org/x/tools/imports"
)

const runtime = "tinyactors.dev/precompile/tinyactors"

type table struct {
	Charts []struct {
		Name        string `json:"name"`
		Datamodel   string `json:"datamodel"`
		Fingerprint string `json:"fingerprint"`
		Data        []struct {
			ID         string `json:"id"`
			Expression *int   `json:"expression"`
		} `json:"data"`
		Expressions []expression `json:"expressions"`
	} `json:"charts"`
}

type expression struct {
	Index  int    `json:"index"`
	Kind   string `json:"kind"`
	Line   int    `json:"line"`
	Column int    `json:"column"`
	Text   string `json:"text"`
}

// The names a chart's code reads besides its variables, and what they are
// in the generated code.
var system = map[string]string{"_event": "Event", "_sessionid": "SessionID", "_name": "Name", "_ioprocessors": "IOProcessors", "In": "In"}

func main() {
	tool := flag.String("table", os.Getenv("TINYACTORS_TABLE"), "the ta-precompile-table program")
	flag.Parse()
	if flag.NArg() != 1 || *tool == "" {
		fmt.Fprintln(os.Stderr, "usage: tinyactors-gen -table ta-precompile-table chart.scxml")
		os.Exit(2)
	}
	if err := generate(*tool, flag.Arg(0)); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func generate(tool, chartPath string) error {
	raw, err := exec.Command(tool, chartPath).Output()
	if err != nil {
		if exit, ok := err.(*exec.ExitError); ok {
			return fmt.Errorf("%s%v", exit.Stderr, err)
		}
		return err
	}
	var t table
	if err := json.Unmarshal(raw, &t); err != nil {
		return err
	}
	for _, chart := range t.Charts {
		if chart.Datamodel != "go" {
			continue
		}
		out := strings.TrimSuffix(chartPath, filepath.Ext(chartPath)) + "_gen.go"
		code, err := chartCode(chartPath, out, chart.Name, chart.Fingerprint, chart.Data, chart.Expressions)
		if err != nil {
			return err
		}
		if err := os.WriteFile(out, code, 0o644); err != nil {
			return err
		}
	}
	return nil
}

type variable struct {
	id   string
	expr *int
	typ  types.Type
}

func chartCode(chartPath, out, name, fingerprint string, data []struct {
	ID         string `json:"id"`
	Expression *int   `json:"expression"`
}, expressions []expression) ([]byte, error) {
	dir := filepath.Dir(out)
	absolute, _ := filepath.Abs(dir)
	// The package's own imports, so the expressions may use them.
	pkgImports := map[string]bool{}
	files, _ := filepath.Glob(filepath.Join(dir, "*.go"))
	packageName := "main"
	for _, f := range files {
		if filepath.Base(f) == filepath.Base(out) {
			continue
		}
		parsed, err := parser.ParseFile(token.NewFileSet(), f, nil, parser.ImportsOnly)
		if err != nil {
			continue
		}
		packageName = parsed.Name.Name
		for _, spec := range parsed.Imports {
			pkgImports[spec.Path.Value] = true
		}
	}
	// The check: one function whose locals are the variables and whose
	// statements are the expressions, each on its own lines.
	var check bytes.Buffer
	fmt.Fprintf(&check, "package %s\n\nimport (\n\ttinyactors %q\n", packageName, runtime)
	for path := range pkgImports {
		if path != strconv.Quote(runtime) {
			fmt.Fprintf(&check, "\t%s\n", path)
		}
	}
	check.WriteString(")\n\nfunc _tinyactorsCheck(_event tinyactors.Event, _sessionid string, _name string, _ioprocessors map[string]string, In func(string) bool) {\n")
	variables := make([]variable, len(data))
	for i, d := range data {
		variables[i] = variable{id: d.ID, expr: d.Expression}
		if d.Expression == nil {
			fmt.Fprintf(&check, "\tvar %s tinyactors.Value\n", d.ID)
		} else {
			fmt.Fprintf(&check, "\tvar %s = %s\n", d.ID, expressions[*d.Expression].Text)
		}
		fmt.Fprintf(&check, "\t_ = %s\n", d.ID)
	}
	lines := map[int]int{} // a line of the check -> the expression on it
	for _, x := range expressions {
		line := strings.Count(check.String(), "\n") + 1
		switch x.Kind {
		case "condition":
			fmt.Fprintf(&check, "\tif %s {\n\t}\n", x.Text)
		case "value", "content":
			fmt.Fprintf(&check, "\t_ = %s\n", x.Text)
		case "location", "item":
			fmt.Fprintf(&check, "\t%s = %s\n", x.Text, x.Text)
		case "script", "global_script":
			fmt.Fprintf(&check, "\t{\n%s\n\t}\n", x.Text)
		case "string":
			fmt.Fprintf(&check, "\t_ = %s\n", x.Text)
		default:
			continue
		}
		for l := line; l <= strings.Count(check.String(), "\n"); l++ {
			lines[l] = x.Index
		}
	}
	check.WriteString("}\n")
	checkFile := filepath.Join(absolute, "zz_tinyactors_check.go")
	// the packages an expression names, imported as goimports would
	// (only adding: every line keeps its place, which the errors need)
	checked, err := imports.Process(checkFile, check.Bytes(), &imports.Options{Comments: true, TabIndent: true, TabWidth: 8})
	if err != nil {
		checked = check.Bytes()
	}
	offset := strings.Count(string(checked), "\n") - strings.Count(check.String(), "\n")
	if offset != 0 {
		shifted := map[int]int{}
		for line, index := range lines {
			shifted[line+offset] = index
		}
		lines = shifted
	}
	overlay := map[string][]byte{checkFile: checked}
	if abs, _ := filepath.Abs(out); fileExists(abs) {
		overlay[abs] = []byte("package " + packageName + "\n")
	}
	config := &packages.Config{
		Mode:    packages.NeedName | packages.NeedTypes | packages.NeedTypesInfo | packages.NeedSyntax | packages.NeedFiles,
		Dir:     absolute,
		Overlay: overlay,
	}
	loaded, err := packages.Load(config, ".")
	if err != nil {
		return nil, err
	}
	pkg := loaded[0]
	var problems []string
	for _, e := range pkg.Errors {
		position, message := e.Pos, e.Msg
		if !strings.Contains(position, "zz_tinyactors_check.go") || strings.Contains(message, "declared and not used") ||
			strings.Contains(message, "imported and not used") {
			continue
		}
		parts := strings.Split(position, ":")
		line, _ := strconv.Atoi(parts[len(parts)-2])
		if index, ok := lines[line]; ok {
			x := expressions[index]
			problems = append(problems, fmt.Sprintf("%s:%d:%d: %s (in %q)", chartPath, x.Line, x.Column, message, x.Text))
		} else {
			problems = append(problems, fmt.Sprintf("%s: %s", chartPath, message))
		}
	}
	if len(problems) > 0 {
		sort.Strings(problems)
		return nil, fmt.Errorf("%s", strings.Join(problems, "\n"))
	}
	var file *ast.File
	for _, f := range pkg.Syntax {
		if pkg.Fset.Position(f.Pos()).Filename == checkFile {
			file = f
		}
	}
	function := file.Decls[len(file.Decls)-1].(*ast.FuncDecl)
	info := pkg.TypesInfo
	// what each local of the check stands for in the generated code
	fields := map[types.Object]string{}
	for _, param := range function.Type.Params.List {
		for _, n := range param.Names {
			fields[info.Defs[n]] = system[n.Name]
		}
	}
	byLine := map[int][]ast.Stmt{}
	for _, statement := range function.Body.List {
		line := pkg.Fset.Position(statement.Pos()).Line
		byLine[line] = append(byLine[line], statement)
		if declaration, ok := statement.(*ast.DeclStmt); ok {
			for _, spec := range declaration.Decl.(*ast.GenDecl).Specs {
				for _, n := range spec.(*ast.ValueSpec).Names {
					object := info.Defs[n]
					fields[object] = n.Name
					for i := range variables {
						if variables[i].id == n.Name {
							variables[i].typ = object.Type()
						}
					}
				}
			}
		}
	}
	qualifier := func(p *types.Package) string {
		if p == pkg.Types {
			return ""
		}
		return p.Name()
	}
	// an expression of the check, its locals read from the session
	render := func(node ast.Node) string {
		rewritten := astutil.Apply(node, nil, func(c *astutil.Cursor) bool {
			if ident, ok := c.Node().(*ast.Ident); ok {
				if field, ok := fields[info.Uses[ident]]; ok {
					if _, selected := c.Parent().(*ast.SelectorExpr); selected && c.Name() == "Sel" {
						return true
					}
					c.Replace(&ast.SelectorExpr{X: ast.NewIdent("s"), Sel: ast.NewIdent(field)})
				}
			}
			return true
		})
		var b bytes.Buffer
		printer.Fprint(&b, token.NewFileSet(), rewritten)
		return b.String()
	}
	statementsOf := map[int][]ast.Stmt{}
	for line, index := range lines {
		if list, ok := byLine[line]; ok {
			statementsOf[index] = append(statementsOf[index], list...)
		}
	}
	typeOf := func(e ast.Expr) string { return types.TypeString(info.TypeOf(e), qualifier) }
	session := name + "Session"
	g := &generated{chart: filepath.Base(chartPath), out: filepath.Base(out)}
	g.printf("// Code generated by tinyactors-gen from %s; DO NOT EDIT.\n\n", filepath.Base(chartPath))
	g.printf("package %s\n\nimport (\n\t\"github.com/fxamacker/cbor/v2\"\n\ttinyactors %q\n)\n\n", packageName, runtime)
	g.printf("// %s is a session of the chart %q: its variables, and its expressions as Go.\n", session, name)
	g.printf("type %s struct {\n\ttinyactors.Base\n", session)
	for _, v := range variables {
		g.printf("\t%s %s\n", v.id, types.TypeString(v.typ, qualifier))
	}
	g.printf("}\n\n")
	ids := make([]string, len(variables))
	for i, v := range variables {
		ids[i] = strconv.Quote(v.id)
	}
	g.printf("var _ = tinyactors.Register(tinyactors.Chart{\n\tName: %q,\n\tFingerprint: 0x%s,\n\tData: []string{%s},\n\tNew: func() tinyactors.Session { return &%s{} },\n})\n\n",
		name, fingerprint, strings.Join(ids, ", "), session)
	// <data>: initialize, or set from a spawn's params
	g.printf("func (s *%s) InitData(declaration int) {\n\tswitch declaration {\n", session)
	for i, v := range variables {
		if v.expr == nil {
			continue
		}
		x := expressions[*v.expr]
		g.printf("\tcase %d:\n", i)
		g.at(x, fmt.Sprintf("s.%s = %s", v.id, render(declarationValue(byLine, pkg.Fset, v.id))))
	}
	g.printf("\t}\n}\n\n")
	g.printf("func (s *%s) SetData(declaration int, value any) {\n\tswitch declaration {\n", session)
	for i, v := range variables {
		g.printf("\tcase %d:\n\t\ts.%s = tinyactors.As[%s](value)\n", i, v.id, types.TypeString(v.typ, qualifier))
	}
	g.printf("\t}\n}\n\n")
	// one switch per kind
	cases := func(kinds []string, each func(x expression, statements []ast.Stmt)) {
		for _, x := range expressions {
			for _, kind := range kinds {
				if x.Kind == kind {
					each(x, statementsOf[x.Index])
				}
			}
		}
	}
	g.printf("func (s *%s) Condition(expression int) bool {\n\tswitch expression {\n", session)
	cases([]string{"condition"}, func(x expression, statements []ast.Stmt) {
		g.printf("\tcase %d:\n", x.Index)
		g.at(x, "return "+render(statements[0].(*ast.IfStmt).Cond))
	})
	g.printf("\t}\n\tpanic(tinyactors.NoExpression(expression))\n}\n\n")
	valueOf := func(statements []ast.Stmt) ast.Expr {
		assignment := statements[0].(*ast.AssignStmt)
		return assignment.Rhs[0]
	}
	g.printf("func (s *%s) Value(expression int) any {\n\tswitch expression {\n", session)
	cases([]string{"value", "content", "location", "item", "string"}, func(x expression, statements []ast.Stmt) {
		g.printf("\tcase %d:\n", x.Index)
		g.at(x, "return "+render(valueOf(statements)))
	})
	g.printf("\t}\n\tpanic(tinyactors.NoExpression(expression))\n}\n\n")
	g.printf("func (s *%s) String(expression int) string {\n\tswitch expression {\n", session)
	cases([]string{"value", "content", "location", "string"}, func(x expression, statements []ast.Stmt) {
		g.printf("\tcase %d:\n", x.Index)
		g.at(x, "return tinyactors.Text("+render(valueOf(statements))+")")
	})
	g.printf("\t}\n\tpanic(tinyactors.NoExpression(expression))\n}\n\n")
	g.printf("func (s *%s) Assign(location int, value any) {\n\tswitch location {\n", session)
	cases([]string{"location", "item"}, func(x expression, statements []ast.Stmt) {
		target := statements[0].(*ast.AssignStmt).Lhs[0]
		g.printf("\tcase %d:\n", x.Index)
		g.at(x, fmt.Sprintf("%s = tinyactors.As[%s](value)", render(target), typeOf(target)))
		g.printf("\t\treturn\n")
	})
	g.printf("\t}\n\tpanic(tinyactors.NoExpression(location))\n}\n\n")
	g.printf("func (s *%s) Script(expression int) {\n\tswitch expression {\n", session)
	cases([]string{"script", "global_script"}, func(x expression, statements []ast.Stmt) {
		g.printf("\tcase %d:\n", x.Index)
		for _, statement := range statements[0].(*ast.BlockStmt).List {
			g.at(x, render(statement))
		}
		g.printf("\t\treturn\n")
	})
	g.printf("\t}\n\tpanic(tinyactors.NoExpression(expression))\n}\n\n")
	g.printf("func (s *%s) Variables() map[string]any {\n\treturn map[string]any{", session)
	for i, v := range variables {
		if i > 0 {
			g.printf(", ")
		}
		g.printf("%q: s.%s", v.id, v.id)
	}
	g.printf("}\n}\n\n")
	g.printf("func (s *%s) SetVariables(variables map[string]cbor.RawMessage) {\n", session)
	for _, v := range variables {
		g.printf("\ttinyactors.Decode(variables, %q, &s.%s)\n", v.id, v.id)
	}
	g.printf("}\n")
	formatted, err := imports.Process(out, g.b.Bytes(), &imports.Options{Comments: true, TabIndent: true, TabWidth: 8, FormatOnly: false})
	if err != nil {
		return nil, fmt.Errorf("%v\n%s", err, g.b.String())
	}
	formatted, err = format.Source(formatted)
	if err != nil {
		return nil, err
	}
	return g.resolve(formatted), nil
}

// the value a <data> local of the check was declared with
func declarationValue(byLine map[int][]ast.Stmt, fset *token.FileSet, id string) ast.Expr {
	for _, list := range byLine {
		for _, statement := range list {
			if declaration, ok := statement.(*ast.DeclStmt); ok {
				spec := declaration.Decl.(*ast.GenDecl).Specs[0].(*ast.ValueSpec)
				if spec.Names[0].Name == id && len(spec.Values) == 1 {
					return spec.Values[0]
				}
			}
		}
	}
	return ast.NewIdent("nil")
}

// The generated file, with //line directives around each expression so
// that errors and panics name the chart's line.
type generated struct {
	b          bytes.Buffer
	chart, out string
}

func (g *generated) printf(format string, args ...any) { fmt.Fprintf(&g.b, format, args...) }

func (g *generated) at(x expression, statement string) {
	g.printf("//line %s:%d\n\t\t%s\n//line %s:1\n", g.chart, x.Line, statement, g.out)
}

// After formatting: each directive back to the generated file names the
// line after it.
func (g *generated) resolve(code []byte) []byte {
	lines := strings.Split(string(code), "\n")
	reset := "//line " + g.out + ":"
	for i, line := range lines {
		if strings.HasPrefix(line, reset) {
			lines[i] = fmt.Sprintf("//line %s:%d", g.out, i+2)
		}
	}
	return []byte(strings.Join(lines, "\n"))
}

func fileExists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}
