// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package bindings2

// TODO(mknyszek): Support unions, handles, interfaces, strings, and vectors.

import (
	"errors"
	"fmt"
	"math"
	"reflect"
	"syscall/zx"
)

// align increases size such that size is aligned to bytes, and returns the new size.
//
// bytes must be a power of 2.
func align(size, bytes int) int {
	offset := size & (bytes - 1)
	// If we're not currently aligned to |bytes| bytes, add padding.
	if offset != 0 {
		size += (bytes - offset)
	}
	return size
}

// encoder represents the encoding context that is necessary to maintain across
// recursive calls within the same FIDL object.
type encoder struct {
	head int

	// buffer represents the output buffer that the encoder writes into.
	buffer []byte

	// handles are the handles discovered when traversing the FIDL data
	// structure. They are referenced from within the serialized data
	// structure in buffer.
	handles []zx.Handle
}

func (e *encoder) newObject(size int) int {
	size = align(size, 8)
	start := len(e.buffer)
	e.buffer = append(e.buffer, make([]byte, size)...)
	return start
}

// writeInt writes an integer of byte-width size to the buffer.
//
// Before writing, it pads the buffer such that the integer is aligned to
// its own byte-width.
//
// size must be a power of 2 <= 8.
func (e *encoder) writeInt(val int64, size int) {
	e.writeUint(uint64(val), size)
}

// writeUint writes an unsigned integer of byte-width size to the buffer.
//
// Before writing, it pads the buffer such that the integer is aligned to
// its own byte-width.
//
// size must be a power of 2 <= 8.
func (e *encoder) writeUint(val uint64, size int) {
	e.head = align(e.head, size)
	for i := e.head; i < e.head+size; i++ {
		e.buffer[i] = byte(val & 0xFF)
		val >>= 8
	}
	e.head += size
}

// marshal is the central recursive function core to marshalling, and
// traverses the tree-like structure of the input type t. v represents
// the value associated with the type t.
//
// It marshals only exported struct fields.
func (e *encoder) marshal(t reflect.Type, v reflect.Value) error {
	switch t.Kind() {
	case reflect.Array:
		elemType := t.Elem()
		for i := 0; i < t.Len(); i++ {
			if err := e.marshal(elemType, v.Index(i)); err != nil {
				return err
			}
		}
	case reflect.Bool:
		// Encodes bools with 1 byte, just like FIDL.
		i := uint64(0)
		if v.Bool() {
			i = 1
		}
		e.writeUint(i, 1)
	case reflect.Int8:
		e.writeInt(v.Int(), 1)
	case reflect.Int16:
		e.writeInt(v.Int(), 2)
	case reflect.Int32:
		e.writeInt(v.Int(), 4)
	case reflect.Int64:
		e.writeInt(v.Int(), 8)
	case reflect.Uint8:
		e.writeUint(v.Uint(), 1)
	case reflect.Uint16:
		e.writeUint(v.Uint(), 2)
	case reflect.Uint32:
		e.writeUint(v.Uint(), 4)
	case reflect.Uint64:
		e.writeUint(v.Uint(), 8)
	case reflect.Float32:
		e.writeUint(uint64(math.Float32bits(float32(v.Float()))), 4)
	case reflect.Float64:
		e.writeUint(math.Float64bits(v.Float()), 8)
	case reflect.Struct:
		// Get the alignment for the struct, and then align to it.
		//
		// Note that Addr can fail if the originally derived value is not "addressable",
		// meaning the root ValueOf() call was on a struct value, not a pointer. However,
		// we guarantee the struct is addressable by forcing a Payload to be passed in
		// (a struct value will never cast as an interface).
		//
		// We avoid using Implements(), MethodByName(), and Call() here because they're
		// very slow.
		payload, ok := v.Addr().Interface().(Payload)
		if !ok {
			return fmt.Errorf("struct %s must implement Payload", t.Name())
		}
		e.head = align(e.head, payload.InlineAlignment())
		for i := 0; i < t.NumField(); i++ {
			f := t.Field(i)
			// If it's an unexported field, ignore it.
			if f.PkgPath != "" {
				continue
			}
			if err := e.marshal(f.Type, v.Field(i)); err != nil {
				return err
			}
		}
	default:
		return fmt.Errorf("unsupported type kind %s for type %s", t.Kind(), t.Name())
	}
	return nil
}

func marshalHeader(header *MessageHeader) []byte {
	e := encoder{}
	e.head = e.newObject(MessageHeaderSize)
	e.writeUint(uint64(header.Txid), 4)
	e.writeUint(uint64(header.Reserved), 4)
	e.writeUint(uint64(header.Flags), 4)
	e.writeUint(uint64(header.Ordinal), 4)
	return e.buffer
}

// Marshal returns the FIDL encoding of a FIDL message comprised of a header
// and a payload which lies in s.
//
// s must be a pointer to a struct, since the primary object in a FIDL message
// is always a struct.
//
// Marshal traverses the value s recursively, following nested type values via
// reflection in order to encode the FIDL struct.
func Marshal(header *MessageHeader, s Payload) ([]byte, []zx.Handle, error) {
	// First, let's make sure we have the right type in s.
	t := reflect.TypeOf(s)
	if t.Kind() != reflect.Ptr {
		return nil, nil, errors.New("expected a pointer")
	}
	t = t.Elem()
	if t.Kind() != reflect.Struct {
		return nil, nil, errors.New("primary object must be a struct")
	}

	// Now, let's get the value of s, marshal the header into a starting
	// buffer, and then marshal the rest of the payload in s.
	v := reflect.ValueOf(s).Elem()
	e := encoder{buffer: marshalHeader(header)}
	e.head = e.newObject(s.InlineSize())
	if err := e.marshal(t, v); err != nil {
		return nil, nil, err
	}
	return e.buffer, e.handles, nil
}

// decoder represents the decoding context that is necessary to maintain
// across recursive calls within the same FIDL object.
type decoder struct {
	head int

	// buffer represents the buffer we're decoding from.
	buffer []byte
}

// readInt reads a signed integer value of byte-width size from the buffer.
//
// Before it reads, however, it moves the head forward so as to be naturally
// aligned with the byte-width of the integer it is reading.
//
// size must be a power of 2 <= 8.
func (d *decoder) readInt(size int) int64 {
	return int64(d.readUint(size))
}

// readUint reads an unsigned integer value of byte-width size from the buffer.
//
// Before it reads, however, it moves the head forward so as to be naturally
// aligned with the byte-width of the integer it is reading.
//
// size must be a power of 2 <= 8.
func (d *decoder) readUint(size int) uint64 {
	d.head = align(d.head, size)
	var val uint64
	for i := d.head + size - 1; i >= d.head; i-- {
		val <<= 8
		val |= uint64(d.buffer[i])
	}
	d.head += size
	return val
}

// unmarshal is the central recursive function core to unmarshalling, and
// traverses the tree-like structure of the input type t. v represents
// the value associated with the type t.
//
// It unmarshals only exported struct fields.
func (d *decoder) unmarshal(t reflect.Type, v reflect.Value) error {
	switch t.Kind() {
	case reflect.Array:
		elemType := t.Elem()
		for i := 0; i < t.Len(); i++ {
			if err := d.unmarshal(elemType, v.Index(i)); err != nil {
				return err
			}
		}
	case reflect.Bool:
		i := d.readUint(1)
		switch i {
		case 0:
			v.SetBool(false)
		case 1:
			v.SetBool(true)
		default:
			return fmt.Errorf("%d is not a valid bool value", i)
		}
	case reflect.Int8:
		v.SetInt(d.readInt(1))
	case reflect.Int16:
		v.SetInt(d.readInt(2))
	case reflect.Int32:
		v.SetInt(d.readInt(4))
	case reflect.Int64:
		v.SetInt(d.readInt(8))
	case reflect.Uint8:
		v.SetUint(d.readUint(1))
	case reflect.Uint16:
		v.SetUint(d.readUint(2))
	case reflect.Uint32:
		v.SetUint(d.readUint(4))
	case reflect.Uint64:
		v.SetUint(d.readUint(8))
	case reflect.Float32:
		v.SetFloat(float64(math.Float32frombits(uint32(d.readUint(4)))))
	case reflect.Float64:
		v.SetFloat(math.Float64frombits(d.readUint(8)))
	case reflect.Struct:
		// Get the alignment for the struct, and then align to it.
		//
		// Note that Addr can fail if the originally derived value is not "addressable",
		// meaning the root ValueOf() call was on a struct value, not a pointer. However,
		// we guarantee the struct is addressable by forcing a Payload to be passed in
		// (a struct value will never cast as an interface).
		//
		// We avoid using Implements(), MethodByName(), and Call() here because they're
		// very slow.
		payload, ok := v.Addr().Interface().(Payload)
		if !ok {
			return fmt.Errorf("struct %s must implement Payload", t.Name())
		}
		d.head = align(d.head, payload.InlineAlignment())
		for i := 0; i < t.NumField(); i++ {
			f := t.Field(i)
			// If it's an unexported field, ignore it.
			if f.PkgPath != "" {
				continue
			}
			if err := d.unmarshal(f.Type, v.Field(i)); err != nil {
				return err
			}
		}
	default:
		return fmt.Errorf("unsupported type kind %s", t.Kind())
	}
	return nil
}

func unmarshalHeader(data []byte, m *MessageHeader) error {
	if len(data) < 16 {
		return fmt.Errorf("too few bytes in payload to parse header")
	}
	d := decoder{buffer: data}
	m.Txid = uint32(d.readUint(4))
	m.Reserved = uint32(d.readUint(4))
	m.Flags = uint32(d.readUint(4))
	m.Ordinal = uint32(d.readUint(4))
	return nil
}

// Unmarshal parses the encoded FIDL message in data, storing the decoded payload
// in s and returning the message header.
//
// The value pointed to by s must be a pointer to a golang struct which represents
// the decoded primary object of a FIDL message. The data decode process is guided
// by the structure of the struct pointed to by s.
//
// TODO(mknyszek): More rigorously validate the input.
func Unmarshal(data []byte, _ []zx.Handle, s Payload) (*MessageHeader, error) {
	// First, let's make sure we have the right type in s.
	t := reflect.TypeOf(s)
	if t.Kind() != reflect.Ptr {
		return nil, errors.New("expected a pointer")
	}
	t = t.Elem()
	if t.Kind() != reflect.Struct {
		return nil, errors.New("primary object must be a struct")
	}

	// Since that succeeded, let's unmarshal the header.
	var m MessageHeader
	if err := unmarshalHeader(data, &m); err != nil {
		return nil, err
	}

	// Get the payload's value and unmarshal it.
	d := decoder{buffer: data[MessageHeaderSize:]}
	return &m, d.unmarshal(t, reflect.ValueOf(s).Elem())
}
