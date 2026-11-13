package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/url"
	"sync"
)

type updateControl struct {
	encoder *json.Encoder
	output  *sync.Mutex

	mu      sync.Mutex
	nextID  int
	pending map[int]chan proxyResponse
}

type proxyResponse struct {
	Action string `json:"action"`
	ID     int    `json:"id"`
	Proxy  string `json:"proxy"`
}

func newUpdateControl(parent context.Context, input io.Reader, encoder *json.Encoder, output *sync.Mutex) (*updateControl, context.Context) {
	ctx, cancel := context.WithCancel(parent)
	control := &updateControl{encoder: encoder, output: output, pending: make(map[int]chan proxyResponse)}
	go func() {
		defer cancel()
		decoder := json.NewDecoder(input)
		for {
			var response proxyResponse
			if decoder.Decode(&response) != nil || response.Action == "cancel" {
				return
			}
			control.mu.Lock()
			if reply := control.pending[response.ID]; reply != nil {
				select {
				case reply <- response:
				default:
				}
			}
			control.mu.Unlock()
		}
	}()
	return control, ctx
}

func (c *updateControl) proxy(request *http.Request) (*url.URL, error) {
	ctx := request.Context()
	if err := ctx.Err(); err != nil {
		return nil, err
	}

	reply := make(chan proxyResponse, 1)
	c.mu.Lock()
	c.nextID++
	id := c.nextID
	c.pending[id] = reply
	c.mu.Unlock()
	defer func() {
		c.mu.Lock()
		delete(c.pending, id)
		c.mu.Unlock()
	}()

	c.output.Lock()
	err := c.encoder.Encode(struct {
		Type string `json:"type"`
		ID   int    `json:"request_id"`
		URL  string `json:"url"`
	}{"proxy", id, request.URL.String()})
	c.output.Unlock()
	if err != nil {
		return nil, err
	}

	select {
	case <-ctx.Done():
		return nil, ctx.Err()
	case response := <-reply:
		switch response.Action {
		case "environment":
			return http.ProxyFromEnvironment(request)
		case "proxy":
			if response.Proxy == "" {
				return nil, nil
			}
			proxy, err := url.Parse(response.Proxy)
			if err != nil || proxy.Hostname() == "" {
				return nil, errors.New("invalid system proxy address")
			}
			switch proxy.Scheme {
			case "http", "https", "socks5", "socks5h":
				return proxy, nil
			}
		}
		return nil, errors.New("system proxy uses an unsupported protocol")
	}
}
