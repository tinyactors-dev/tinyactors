// The application's own types and functions, which the chart's Go
// expressions use as any Go code in the package would.
package main

import "tinyactors.dev/precompile/tinyactors"

type Item struct {
	SKU      string `cbor:"sku"`
	Quantity int    `cbor:"quantity"`
	Cents    int    `cbor:"cents"`
}

type Address struct {
	Country string `cbor:"country"`
}

type Catalog struct{ prices map[string]int }

var catalog = Catalog{prices: map[string]int{"tea": 450, "mug": 1200}}

// knows says whether an event's data names an item the shop sells.
func (c Catalog) knows(data tinyactors.Value) bool {
	var item Item
	if data.Decode(&item) != nil || item.Quantity < 1 {
		return false
	}
	_, ok := c.prices[item.SKU]
	return ok
}

// item is the line an event's data asks for, priced.
func (c Catalog) item(data tinyactors.Value) Item {
	item := tinyactors.As[Item](data)
	item.Cents = c.prices[item.SKU]
	return item
}
