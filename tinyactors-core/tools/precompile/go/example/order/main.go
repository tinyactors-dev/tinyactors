// The order example in Go: the chart's code is generated into order_gen.go.
package main

//go:generate go run tinyactors.dev/precompile/cmd/tinyactors-gen order.scxml

import (
	"os"

	"tinyactors.dev/precompile/tinyactors"
)

func main() { os.Exit(tinyactors.RunOrderDemo("order.scxml")) }
