// The cost per call of a Go function behind the json datamodel's function
// ABI (tinyactors/datamodel_json.h: ta_json_function, ta_json_call), called
// from C through cgo, beside the same functions written in C.
//
//	go run . [-n 1000000] [-runs 5]
package main

/*
#cgo CFLAGS: -O2 -I${SRCDIR}/../../../../include
#cgo LDFLAGS: -L${SRCDIR}/../../../../build/release/lib -ltinyactors_base
#include <stdlib.h>
#include "driver.h"
*/
import "C"

import (
	"bytes"
	"flag"
	"fmt"
	"os"
	"runtime"
	"runtime/cgo"
	"unsafe"

	"github.com/fxamacker/cbor/v2"
)

// The one exported trampoline: the ABI's signature; the context is a
// cgo.Handle of the Go closure.
//
//export jb_go_trampoline
func jb_go_trampoline(call *C.ta_json_call) C.ta_status {
	f := cgo.Handle(uintptr(call.context)).Value().(func(*C.ta_json_call) C.ta_status)
	return f(call)
}

type Item struct {
	Sku   string `cbor:"sku"`
	Qty   uint64 `cbor:"qty"`
	Cents uint64 `cbor:"cents"`
}

type Add struct {
	Sku string `cbor:"sku"`
	Qty uint64 `cbor:"qty"`
}

type hasItemsArgs struct {
	_     struct{} `cbor:",toarray"`
	Items []Item
}

type addItemArgs struct {
	_     struct{} `cbor:",toarray"`
	Items []Item
	Add   Add
}

var (
	dm, _ = cbor.DecOptions{ExtraReturnErrors: cbor.ExtraDecErrorUnknownField}.DecMode()
	em, _ = cbor.EncOptions{}.EncMode()
)

// The call's arguments: a view of C memory, valid during the call. The
// decoder copies what it keeps (strings), so nothing aliases it after.
func arguments(call *C.ta_json_call) []byte {
	return unsafe.Slice((*byte)(call.arguments.pointer), int(call.arguments.length))
}

func writeTrue(call *C.ta_json_call, v bool) {
	if v {
		C.ta_cbor_write_simple(call.result, C.TA_CBOR_TRUE)
	} else {
		C.ta_cbor_write_simple(call.result, C.TA_CBOR_FALSE)
	}
}

// Encoded bytes into the writer: a Go pointer to memory holding no Go
// pointers, passed for the call only, as cgo's rules allow.
func writeEncoded(call *C.ta_json_call, b []byte) C.ta_status {
	return C.jb_write_item(call.result, C.ta_bytes{pointer: unsafe.Pointer(&b[0]), length: C.size_t(len(b))})
}

func price(sku string) uint64 {
	switch sku {
	case "tea":
		return 450
	case "mug":
		return 1200
	}
	return 0
}

// The application's functions, as a Go programmer writes them.
func hasItems(items []Item) bool { return len(items) > 0 }

func addItem(items []Item, add Add) []Item {
	return append(items, Item{Sku: add.Sku, Qty: add.Qty, Cents: price(add.Sku)})
}

var goFunctions = []struct {
	name string
	f    func(*C.ta_json_call) C.ta_status
}{
	{"crossing", func(call *C.ta_json_call) C.ta_status { return C.TA_OK }}, // writes nothing: C->Go only
	{"empty", func(call *C.ta_json_call) C.ta_status {
		writeTrue(call, true)
		return C.TA_OK
	}},
	{"hasItems", func(call *C.ta_json_call) C.ta_status {
		var a hasItemsArgs
		if dm.Unmarshal(arguments(call), &a) != nil {
			return C.TA_ERROR_INVALID
		}
		writeTrue(call, hasItems(a.Items))
		return C.TA_OK
	}},
	{"addItem", func(call *C.ta_json_call) C.ta_status {
		var a addItemArgs
		if dm.Unmarshal(arguments(call), &a) != nil {
			return C.TA_ERROR_INVALID
		}
		b, err := em.Marshal(addItem(a.Items, a.Add))
		if err != nil {
			return C.TA_ERROR_INVALID
		}
		return writeEncoded(call, b)
	}},
}

func jbCase(f C.ta_json_function, context unsafe.Pointer, args C.ta_bytes, nothing C.uint32_t) C.jb_case {
	return C.jb_case{function: f, context: context, arguments: args, writes_nothing: nothing}
}

func cBytes(v any) C.ta_bytes {
	b, err := em.Marshal(v)
	if err != nil {
		panic(err)
	}
	return C.ta_bytes{pointer: C.CBytes(b), length: C.size_t(len(b))}
}

type benchCase struct {
	language, name string
	c              C.jb_case
	isGo           bool
}

func main() {
	n := flag.Uint64("n", 1000000, "calls per run")
	runs := flag.Int("runs", 5, "runs; the minimum is reported")
	flag.Parse()

	items := []Item{{"tea", 2, 450}, {"mug", 1, 1200}}
	add := Add{"tea", 3}
	argsEmpty := cBytes([]any{})
	argsHas := cBytes([]any{items})
	argsAdd := cBytes([]any{items, add})
	argFor := map[string]C.ta_bytes{"crossing": argsEmpty, "empty": argsEmpty, "hasItems": argsHas, "addItem": argsAdd}

	// The addItem result, for the re-emit row: what writeEncoded walks.
	resultAdd := (*C.ta_bytes)(C.malloc(C.size_t(unsafe.Sizeof(C.ta_bytes{}))))
	*resultAdd = cBytes(addItem(items, add))

	trampoline := C.ta_json_function(C.jb_go_trampoline)
	cases := []benchCase{
		{"c", "empty", jbCase(C.ta_json_function(C.jb_c_empty), nil, argsEmpty, 0), false},
		{"c", "hasItems", jbCase(C.ta_json_function(C.jb_c_has_items), nil, argsHas, 0), false},
		{"c", "addItem", jbCase(C.ta_json_function(C.jb_c_add_item), nil, argsAdd, 0), false},
		{"c", "reemit(addItem result)", jbCase(C.ta_json_function(C.jb_c_reemit), unsafe.Pointer(resultAdd), argsEmpty, 0), false},
	}
	for _, g := range goFunctions {
		h := cgo.NewHandle(g.f)
		nothing := C.uint32_t(0)
		if g.name == "crossing" {
			nothing = 1
		}
		cases = append(cases, benchCase{"go", g.name, jbCase(trampoline, C.jb_handle_context(C.uintptr_t(h)), argFor[g.name], nothing), true})
	}

	// Check: Go and C write the same bytes.
	want := map[string][]byte{}
	room := C.malloc(4096)
	for _, bc := range cases {
		if bc.name == "crossing" {
			continue
		}
		out := C.ta_buffer{pointer: room, capacity: 4096}
		if s := C.jb_once(&bc.c, &out); s != C.TA_OK {
			fmt.Fprintf(os.Stderr, "%s %s: status %d\n", bc.language, bc.name, int(s))
			os.Exit(1)
		}
		got := C.GoBytes(out.pointer, C.int(out.length))
		if w, ok := want[bc.name]; ok && !bytes.Equal(w, got) {
			fmt.Fprintf(os.Stderr, "%s %s: %x, C wrote %x\n", bc.language, bc.name, got, w)
			os.Exit(1)
		}
		want[bc.name] = got
	}

	for _, bc := range cases {
		best := -1.0
		var ms0, ms1 runtime.MemStats
		runtime.ReadMemStats(&ms0)
		for r := 0; r < *runs; r++ {
			ns := float64(C.jb_time(&bc.c, C.uint64_t(*n)))
			if ns < 0 {
				fmt.Fprintf(os.Stderr, "%s %s: a call failed (%v)\n", bc.language, bc.name, ns)
				os.Exit(1)
			}
			if best < 0 || ns < best {
				best = ns
			}
		}
		runtime.ReadMemStats(&ms1)
		calls := float64(*n) * float64(*runs)
		extra := ""
		if bc.isGo {
			extra = fmt.Sprintf("\t%.1f allocs/call\t%.0f B/call",
				float64(ms1.Mallocs-ms0.Mallocs)/calls, float64(ms1.TotalAlloc-ms0.TotalAlloc)/calls)
		}
		fmt.Printf("%s\t%s\t%.1f ns/call%s\n", bc.language, bc.name, best, extra)
	}
}
