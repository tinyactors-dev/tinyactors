package tinyactors

import (
	"fmt"

	"github.com/fxamacker/cbor/v2"
)

// Value is a value that crossed from another actor or the application: one
// CBOR item, decoded when the chart asks for it as a Go type.
type Value []byte

// MarshalCBOR writes the value as it is (undefined when empty).
func (v Value) MarshalCBOR() ([]byte, error) {
	if len(v) == 0 {
		return []byte{0xf7}, nil
	}
	return v, nil
}

// UnmarshalCBOR keeps the item's bytes.
func (v *Value) UnmarshalCBOR(b []byte) error {
	*v = append(Value(nil), b...)
	return nil
}

// Decode reads the value into what out points to.
func (v Value) Decode(out any) error { return cbor.Unmarshal(v, out) }

// Get is a map's member (undefined when the value is not a map or lacks it).
func (v Value) Get(key string) Value {
	var m map[string]cbor.RawMessage
	if cbor.Unmarshal(v, &m) != nil {
		return nil
	}
	return Value(m[key])
}

// At is an array's item.
func (v Value) At(i int) Value {
	var a []cbor.RawMessage
	if cbor.Unmarshal(v, &a) != nil || i < 0 || i >= len(a) {
		return nil
	}
	return Value(a[i])
}

// String is the value in CBOR's diagnostic notation.
func (v Value) String() string {
	if len(v) == 0 {
		return "undefined"
	}
	text, err := cbor.Diagnose(v)
	if err != nil {
		return fmt.Sprintf("<%x>", []byte(v))
	}
	return text
}

// As converts what an expression gave to the type of the location it is
// assigned to: the value itself when it is a T, a Value decoded into a T.
// Anything else panics, which the chart sees as error.execution.
func As[T any](x any) T {
	if t, ok := x.(T); ok {
		return t
	}
	var t T
	switch v := x.(type) {
	case Value:
		if err := cbor.Unmarshal(v, &t); err != nil {
			panic(fmt.Sprintf("%v is not a %T: %v", v, t, err))
		}
		return t
	case []byte:
		if err := cbor.Unmarshal(v, &t); err != nil {
			panic(fmt.Sprintf("%x is not a %T: %v", v, t, err))
		}
		return t
	}
	panic(fmt.Sprintf("a %T is not a %T", x, t))
}

// Text is what a <log> or a *expr attribute shows of a value.
func Text(x any) string {
	if s, ok := x.(string); ok {
		return s
	}
	return fmt.Sprint(x)
}

// Event is _event: what the session's current event says.
type Event struct {
	Name       string `cbor:"name"`
	Type       string `cbor:"type"`
	SendID     string `cbor:"sendid"`
	Origin     string `cbor:"origin"`
	OriginType string `cbor:"origintype"`
	InvokeID   string `cbor:"invokeid"`
	Raw        string `cbor:"raw"`
	Data       Value  `cbor:"data"`
}
