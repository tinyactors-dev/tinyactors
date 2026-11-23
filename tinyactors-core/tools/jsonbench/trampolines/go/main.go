// The cost per call of a Go function behind the json datamodel's function
// ABI (tinyactors/datamodel_json.h), called from C through cgo, beside the
// same functions written in C, in three result modes, each with the
// driver's own call struct (driver.h): writer (jb_writer_call, a
// ta_cbor_writer), bytes (jb_bytes_call, encoded bytes into a ta_buffer
// the driver lends) and bytes+check (bytes, then a check pass in C over
// them, timed with the call).
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
func jb_go_trampoline(call *C.jb_writer_call) C.ta_status {
	f := cgo.Handle(uintptr(call.context)).Value().(func(*C.jb_writer_call) C.ta_status)
	return f(call)
}

// The bytes modes' trampoline: the driver's own call struct (driver.h).
//
//export jb_go_bytes_trampoline
func jb_go_bytes_trampoline(call *C.jb_bytes_call) C.ta_status {
	f := cgo.Handle(uintptr(call.context)).Value().(func(*C.jb_bytes_call) C.ta_status)
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

type newItemArgs struct {
	_   struct{} `cbor:",toarray"`
	Add Add
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
func arguments(call *C.jb_writer_call) []byte {
	return view(call.arguments)
}

func view(b C.ta_bytes) []byte {
	return unsafe.Slice((*byte)(b.pointer), int(b.length))
}

// Encoded bytes into the room the driver lends: a copy into C memory
// through unsafe.Slice, the length set from Go; no call into C.
func putBytes(call *C.jb_bytes_call, b []byte) C.ta_status {
	room := call.result
	if uint64(len(b)) > uint64(room.capacity) {
		room.length = C.size_t(len(b))
		return C.TA_ERROR_NO_MEMORY
	}
	copy(unsafe.Slice((*byte)(room.pointer), int(room.capacity)), b)
	room.length = C.size_t(len(b))
	return C.TA_OK
}

func writeTrue(call *C.jb_writer_call, v bool) {
	if v {
		C.ta_cbor_write_simple(call.result, C.TA_CBOR_TRUE)
	} else {
		C.ta_cbor_write_simple(call.result, C.TA_CBOR_FALSE)
	}
}

// Encoded bytes into the writer: a Go pointer to memory holding no Go
// pointers, passed for the call only, as cgo's rules allow.
func writeEncoded(call *C.jb_writer_call, b []byte) C.ta_status {
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

func newItem(add Add) Item {
	return Item{Sku: add.Sku, Qty: add.Qty, Cents: price(add.Sku)}
}

func addItem(items []Item, add Add) []Item {
	return append(items, Item{Sku: add.Sku, Qty: add.Qty, Cents: price(add.Sku)})
}

var goFunctions = []struct {
	name string
	f    func(*C.jb_writer_call) C.ta_status
}{
	{"crossing", func(call *C.jb_writer_call) C.ta_status { return C.TA_OK }}, // writes nothing: C->Go only
	{"empty", func(call *C.jb_writer_call) C.ta_status {
		writeTrue(call, true)
		return C.TA_OK
	}},
	{"hasItems", func(call *C.jb_writer_call) C.ta_status {
		var a hasItemsArgs
		if dm.Unmarshal(arguments(call), &a) != nil {
			return C.TA_ERROR_INVALID
		}
		writeTrue(call, hasItems(a.Items))
		return C.TA_OK
	}},
	{"addItem", func(call *C.jb_writer_call) C.ta_status {
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
	{"newItem", func(call *C.jb_writer_call) C.ta_status {
		var a newItemArgs
		if dm.Unmarshal(arguments(call), &a) != nil {
			return C.TA_ERROR_INVALID
		}
		b, err := em.Marshal(newItem(a.Add))
		if err != nil {
			return C.TA_ERROR_INVALID
		}
		return writeEncoded(call, b)
	}},
}

// The same functions in the bytes modes: decode, call, Marshal, copy.
func bytesFunction(compute func(args []byte) (any, bool)) func(*C.jb_bytes_call) C.ta_status {
	return func(call *C.jb_bytes_call) C.ta_status {
		v, ok := compute(view(call.arguments))
		if !ok {
			return C.TA_ERROR_INVALID
		}
		b, err := em.Marshal(v)
		if err != nil {
			return C.TA_ERROR_INVALID
		}
		return putBytes(call, b)
	}
}

var goBytesFunctions = []struct {
	name string
	f    func(*C.jb_bytes_call) C.ta_status
}{
	{"crossing", func(call *C.jb_bytes_call) C.ta_status { return C.TA_OK }},
	{"empty", bytesFunction(func([]byte) (any, bool) { return true, true })},
	{"hasItems", bytesFunction(func(args []byte) (any, bool) {
		var a hasItemsArgs
		if dm.Unmarshal(args, &a) != nil {
			return nil, false
		}
		return hasItems(a.Items), true
	})},
	{"addItem", bytesFunction(func(args []byte) (any, bool) {
		var a addItemArgs
		if dm.Unmarshal(args, &a) != nil {
			return nil, false
		}
		return addItem(a.Items, a.Add), true
	})},
	{"newItem", bytesFunction(func(args []byte) (any, bool) {
		var a newItemArgs
		if dm.Unmarshal(args, &a) != nil {
			return nil, false
		}
		return newItem(a.Add), true
	})},
}

var modeNames = []string{"writer", "bytes", "bytes+check"}

func jbCase(f C.jb_writer_function, context unsafe.Pointer, args C.ta_bytes, nothing C.uint32_t) C.jb_case {
	return C.jb_case{function: f, context: context, arguments: args, writes_nothing: nothing, mode: C.JB_WRITER}
}

func jbBytesCase(f C.jb_bytes_function, context unsafe.Pointer, args C.ta_bytes, nothing C.uint32_t, mode C.uint32_t) C.jb_case {
	return C.jb_case{bytes_function: f, context: context, arguments: args, writes_nothing: nothing, mode: mode}
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

func (bc benchCase) mode() string { return modeNames[bc.c.mode] }

func main() {
	n := flag.Uint64("n", 1000000, "calls per run")
	runs := flag.Int("runs", 5, "runs; the minimum is reported")
	flag.Parse()

	items := []Item{{"tea", 2, 450}, {"mug", 1, 1200}}
	add := Add{"tea", 3}
	argsEmpty := cBytes([]any{})
	argsHas := cBytes([]any{items})
	argsAdd := cBytes([]any{items, add})
	argsNew := cBytes([]any{add})
	argFor := map[string]C.ta_bytes{"crossing": argsEmpty, "empty": argsEmpty, "hasItems": argsHas, "addItem": argsAdd, "newItem": argsNew}

	// The addItem result, for the re-emit row: what writeEncoded walks.
	resultAdd := (*C.ta_bytes)(C.malloc(C.size_t(unsafe.Sizeof(C.ta_bytes{}))))
	*resultAdd = cBytes(addItem(items, add))

	trampoline := C.jb_writer_function(C.jb_go_trampoline)
	cases := []benchCase{
		{"c", "empty", jbCase(C.jb_writer_function(C.jb_c_empty), nil, argsEmpty, 0), false},
		{"c", "hasItems", jbCase(C.jb_writer_function(C.jb_c_has_items), nil, argsHas, 0), false},
		{"c", "addItem", jbCase(C.jb_writer_function(C.jb_c_add_item), nil, argsAdd, 0), false},
		{"c", "newItem", jbCase(C.jb_writer_function(C.jb_c_new_item), nil, argsNew, 0), false},
		{"c", "reemit(addItem result)", jbCase(C.jb_writer_function(C.jb_c_reemit), unsafe.Pointer(resultAdd), argsEmpty, 0), false},
	}
	cBytesFunctions := []struct {
		name string
		f    C.jb_bytes_function
	}{
		{"empty", C.jb_bytes_function(C.jb_c_empty_bytes)},
		{"hasItems", C.jb_bytes_function(C.jb_c_has_items_bytes)},
		{"addItem", C.jb_bytes_function(C.jb_c_add_item_bytes)},
		{"newItem", C.jb_bytes_function(C.jb_c_new_item_bytes)},
	}
	for _, mode := range []C.uint32_t{C.JB_BYTES, C.JB_BYTES_CHECK} {
		for _, f := range cBytesFunctions {
			cases = append(cases, benchCase{"c", f.name, jbBytesCase(f.f, nil, argFor[f.name], 0, mode), false})
		}
	}
	for _, g := range goFunctions {
		h := cgo.NewHandle(g.f)
		nothing := C.uint32_t(0)
		if g.name == "crossing" {
			nothing = 1
		}
		cases = append(cases, benchCase{"go", g.name, jbCase(trampoline, C.jb_handle_context(C.uintptr_t(h)), argFor[g.name], nothing), true})
	}
	bytesTrampoline := C.jb_bytes_function(C.jb_go_bytes_trampoline)
	for _, mode := range []C.uint32_t{C.JB_BYTES, C.JB_BYTES_CHECK} {
		for _, g := range goBytesFunctions {
			nothing := C.uint32_t(0)
			if g.name == "crossing" {
				if mode == C.JB_BYTES_CHECK {
					continue // nothing to check
				}
				nothing = 1
			}
			h := cgo.NewHandle(g.f)
			cases = append(cases, benchCase{"go", g.name, jbBytesCase(bytesTrampoline, C.jb_handle_context(C.uintptr_t(h)), argFor[g.name], nothing, mode), true})
		}
	}

	// Check: every language and mode writes the same bytes for a function.
	want := map[string][]byte{}
	room := C.malloc(4096)
	for _, bc := range cases {
		if bc.name == "crossing" {
			continue
		}
		out := C.ta_buffer{pointer: room, capacity: 4096}
		if s := C.jb_once(&bc.c, &out); s != C.TA_OK {
			fmt.Fprintf(os.Stderr, "%s %s %s: status %d\n", bc.language, bc.name, bc.mode(), int(s))
			os.Exit(1)
		}
		got := C.GoBytes(out.pointer, C.int(out.length))
		if w, ok := want[bc.name]; ok && !bytes.Equal(w, got) {
			fmt.Fprintf(os.Stderr, "%s %s %s: %x, the first case wrote %x\n", bc.language, bc.name, bc.mode(), got, w)
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
				fmt.Fprintf(os.Stderr, "%s %s %s: a call failed (%v)\n", bc.language, bc.name, bc.mode(), ns)
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
		fmt.Printf("%s\t%s\t%s\t%.1f ns/call%s\n", bc.language, bc.name, bc.mode(), best, extra)
	}
}
