// Package output prints search hits as plain lines, JSON or CSV.
package output

import (
	"bufio"
	"encoding/csv"
	"encoding/json"
	"fmt"
	"io"
	"iter"
	"slices"
	"strconv"
	"time"

	"eind/internal/index"
)

type Format int

const (
	Plain Format = iota
	JSON
	CSV
)

type Options struct {
	Format       Format
	NullSep      bool // terminate plain lines with NUL for xargs -0
	NameOnly     bool
	ShowSize     bool
	ShowModified bool
	ShowCreated  bool
	Color        bool
}

const timeLayout = "2006-01-02 15:04"

// Write prints hits from a local index, building each record only as it is
// printed.
func Write(w io.Writer, ix *index.Index, hits []uint32, o Options) error {
	return write(w, func(yield func(Record) bool) {
		for _, h := range hits {
			if !yield(RecordOf(ix, h)) {
				return
			}
		}
	}, o)
}

// WriteRecords prints hits that came from the daemon rather than a local index.
func WriteRecords(w io.Writer, records []Record, o Options) error {
	return write(w, slices.Values(records), o)
}

func write(w io.Writer, records iter.Seq[Record], o Options) error {
	bw := bufio.NewWriterSize(w, 64<<10)
	var err error
	switch o.Format {
	case JSON:
		err = writeJSON(bw, records)
	case CSV:
		err = writeCSV(bw, records)
	default:
		err = writePlain(bw, records, o)
	}
	if err != nil {
		return err
	}
	return bw.Flush()
}

func writePlain(w *bufio.Writer, records iter.Seq[Record], o Options) error {
	terminator := byte('\n')
	if o.NullSep {
		terminator = 0
	}
	for r := range records {
		isDir := r.Type == "dir"
		if o.ShowSize {
			if isDir {
				fmt.Fprintf(w, "%9s  ", "<DIR>")
			} else {
				fmt.Fprintf(w, "%9s  ", HumanSize(r.Size))
			}
		}
		if o.ShowModified {
			w.WriteString(formatTime(r.Modified))
			w.WriteString("  ")
		}
		if o.ShowCreated {
			w.WriteString(formatTime(r.Created))
			w.WriteString("  ")
		}
		text := r.Name
		if !o.NameOnly {
			text = r.Path
		}
		if o.Color && isDir {
			w.WriteString("\x1b[1;34m")
			w.WriteString(text)
			w.WriteString("\x1b[0m")
		} else {
			w.WriteString(text)
		}
		if err := w.WriteByte(terminator); err != nil {
			return err
		}
	}
	return nil
}

func formatTime(rfc3339 string) string {
	t, err := time.Parse(time.RFC3339, rfc3339)
	if err != nil {
		return "                "
	}
	return t.Format(timeLayout)
}

// Record is one search hit in the JSON, CSV and socket protocols.
type Record struct {
	Path     string `json:"path"`
	Name     string `json:"name"`
	Type     string `json:"type"`
	Size     int64  `json:"size"`
	Modified string `json:"modified"`
	Created  string `json:"created,omitempty"`
}

func RecordOf(ix *index.Index, h uint32) Record {
	e := ix.Entry(h)
	r := Record{
		Path:     ix.Path(h),
		Name:     e.Name,
		Type:     "file",
		Size:     e.Size,
		Modified: time.Unix(e.Modified, 0).Format(time.RFC3339),
	}
	if e.IsDir {
		r.Type = "dir"
	}
	if e.Created != 0 {
		r.Created = time.Unix(e.Created, 0).Format(time.RFC3339)
	}
	return r
}

func writeJSON(w *bufio.Writer, records iter.Seq[Record]) error {
	w.WriteString("[")
	n := 0
	for r := range records {
		if n > 0 {
			w.WriteString(",")
		}
		n++
		w.WriteString("\n  ")
		buf, err := json.Marshal(r)
		if err != nil {
			return err
		}
		if _, err := w.Write(buf); err != nil {
			return err
		}
	}
	if n > 0 {
		w.WriteString("\n")
	}
	_, err := w.WriteString("]\n")
	return err
}

func writeCSV(w *bufio.Writer, records iter.Seq[Record]) error {
	cw := csv.NewWriter(w)
	if err := cw.Write([]string{"path", "name", "type", "size", "modified", "created"}); err != nil {
		return err
	}
	for r := range records {
		if err := cw.Write([]string{r.Path, r.Name, r.Type, strconv.FormatInt(r.Size, 10), r.Modified, r.Created}); err != nil {
			return err
		}
	}
	cw.Flush()
	return cw.Error()
}

func HumanSize(n int64) string {
	const unit = 1024
	if n < unit {
		return strconv.FormatInt(n, 10) + " B"
	}
	f := float64(n)
	for _, suffix := range []string{"KB", "MB", "GB", "TB", "PB"} {
		f /= unit
		if f < unit {
			if f < 10 {
				return fmt.Sprintf("%.1f %s", f, suffix)
			}
			return fmt.Sprintf("%.0f %s", f, suffix)
		}
	}
	return fmt.Sprintf("%.0f EB", f/unit)
}
