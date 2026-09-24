package server

import (
	"bufio"
	"encoding/json"
	"errors"
	"net"
)

// Client is one connection to a running daemon.
type Client struct {
	conn net.Conn
	r    *bufio.Reader
}

func Dial(socketPath string) (*Client, error) {
	conn, err := net.Dial("unix", socketPath)
	if err != nil {
		return nil, err
	}
	return &Client{conn: conn, r: bufio.NewReaderSize(conn, 1<<20)}, nil
}

func (c *Client) Close() error { return c.conn.Close() }

func (c *Client) Status() (StatusResponse, error) {
	var resp StatusResponse
	err := c.call(Request{Op: "status"}, &resp)
	return resp, err
}

func (c *Client) Search(req Request) (SearchResponse, error) {
	var resp SearchResponse
	err := c.call(req, &resp)
	return resp, err
}

func (c *Client) call(req Request, resp any) error {
	line, err := json.Marshal(req)
	if err != nil {
		return err
	}
	if _, err := c.conn.Write(append(line, '\n')); err != nil {
		return err
	}
	answer, err := c.r.ReadBytes('\n')
	if err != nil {
		return err
	}
	var failure ErrorResponse
	if json.Unmarshal(answer, &failure) == nil && failure.Error != "" {
		return errors.New(failure.Error)
	}
	return json.Unmarshal(answer, resp)
}
