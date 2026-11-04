package main

import (
	"bufio"
	"encoding/binary"
	"io"
	"net"
	"os"
	"syscall"
	"unsafe"
)

// The headset's scratch space (downloads, the image being built) is a sparse file on this PC, served as a
// network block device (NBD, fixed newstyle, one export) over a tunnel the headset asked for ("nbd:scratch").
// The headset formats it and mounts it with discard: what it deletes is given back here (holes).

const scratchSize = 64 << 30 // logical size; only what is written takes room on the PC

const (
	fsctlSetSparse   = 0x000900c4
	fsctlSetZeroData = 0x000980c8
)

func openSparse(path string, size int64) (*os.File, error) {
	f, err := os.OpenFile(path, os.O_RDWR|os.O_CREATE, 0o644)
	if err != nil {
		return nil, err
	}
	var n uint32
	syscall.DeviceIoControl(syscall.Handle(f.Fd()), fsctlSetSparse, nil, 0, nil, 0, &n, nil)
	if st, _ := f.Stat(); st.Size() < size {
		f.Truncate(size)
	}
	return f, nil
}

// zeroRange deallocates a range of the sparse file (NBD trim).
func zeroRange(f *os.File, off, length int64) {
	in := [2]int64{off, off + length}
	var n uint32
	syscall.DeviceIoControl(syscall.Handle(f.Fd()), fsctlSetZeroData, (*byte)(unsafe.Pointer(&in[0])), 16, nil, 0, &n, nil)
}

func serveNBD(c net.Conn, r *bufio.Reader, path string) {
	defer c.Close()
	f, err := openSparse(path, scratchSize)
	if err != nil {
		return
	}
	defer f.Close()
	be := binary.BigEndian
	c.Write(append([]byte("NBDMAGICIHAVEOPT"), 0, 3)) // FIXED_NEWSTYLE | NO_ZEROES
	hdr := make([]byte, 20)
	if _, err := io.ReadFull(r, hdr); err != nil { // client flags (4) + "IHAVEOPT" option header (16)
		return
	}
	if _, err := io.CopyN(io.Discard, r, int64(be.Uint32(hdr[16:]))); err != nil { // export name
		return
	}
	reply := make([]byte, 10)
	be.PutUint64(reply, scratchSize)
	be.PutUint16(reply[8:], 1|4|32) // HAS_FLAGS | SEND_FLUSH | SEND_TRIM
	c.Write(reply)

	req := make([]byte, 28)
	buf := make([]byte, 1<<20)
	w := bufio.NewWriterSize(c, 1<<20)
	for {
		if _, err := io.ReadFull(r, req); err != nil {
			return
		}
		typ, handle := be.Uint16(req[6:]), req[8:16]
		off, length := int64(be.Uint64(req[16:])), int64(be.Uint32(req[24:]))
		head := make([]byte, 16)
		be.PutUint32(head, 0x67446698)
		copy(head[8:], handle)
		if int64(len(buf)) < length {
			buf = make([]byte, length)
		}
		switch typ {
		case 0: // read: holes read as zeros
			b := buf[:length]
			n, _ := f.ReadAt(b, off)
			clear(b[n:])
			w.Write(head)
			w.Write(b)
		case 1: // write
			b := buf[:length]
			if _, err := io.ReadFull(r, b); err != nil {
				return
			}
			if _, err := f.WriteAt(b, off); err != nil {
				be.PutUint32(head[4:], 5) // EIO
			}
			w.Write(head)
		case 2: // disconnect
			w.Flush()
			return
		case 3: // flush
			f.Sync()
			w.Write(head)
		case 4: // trim
			zeroRange(f, off, length)
			w.Write(head)
		default:
			be.PutUint32(head[4:], 22) // EINVAL
			w.Write(head)
		}
		if r.Buffered() == 0 { // reply now unless more requests are already waiting
			if err := w.Flush(); err != nil {
				return
			}
		}
	}
}
