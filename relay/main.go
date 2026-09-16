// SPDX-FileCopyrightText: (C) 2026 LiteKVM Devs
// SPDX-License-Identifier: GPL-2.0
// LiteKVM relay — self-hosted cross-network relay for litekvm-pro.
//
// Protocol (both directions, length-prefixed JSON frames):
//   C->R: {"type":"REGISTER","room":"<token>","role":"host"|"guest"}
//   R->C: {"type":"PEER_JOINED"}          (to the first peer when 2nd joins)
//   then: raw bytes are relayed 1:1 between the two peers of a room.
//
// Deploy: go build -o litekvm-relay . && ./litekvm-relay -port 25910
// Or: docker compose up -d   (see relay/docker-compose.yml)
package main

import (
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"sync"
	"time"
)

type room struct {
	peers   map[net.Conn]string // conn -> role
	created time.Time
}

var (
	rooms  = make(map[string]*room)
	roomsM sync.Mutex
)

// joinRoom atomically (under a single roomsM hold) looks up or creates the
// room, enforces capacity and one-host/one-guest, inserts conn, and returns
// the room, current peer count, and the existing peer to notify (nil if none).
// Doing lookup + checks + insert in one critical section removes the TOCTOU
// between getRoom and registration (a concurrent gcRoom could previously
// delete the token in between, splitting joiners of the same token across
// two different room objects).
func joinRoom(token, role string, conn net.Conn) (*room, int, net.Conn, int) {
	roomsM.Lock()
	defer roomsM.Unlock()
	r, ok := rooms[token]
	if !ok {
		r = &room{peers: make(map[net.Conn]string), created: time.Now()}
		rooms[token] = r
		go gcRoom(token)
	}
	if len(r.peers) >= 2 {
		return r, 0, nil, 1 // full
	}
	for _, existingRole := range r.peers {
		if existingRole == role {
			return r, 0, nil, 2 // role taken
		}
	}
	r.peers[conn] = role
	npeers := len(r.peers)
	var other net.Conn
	for peerConn := range r.peers {
		if peerConn != conn {
			other = peerConn
		}
	}
	return r, npeers, other, 0
}

// gcRoom removes empty rooms after 10 minutes of inactivity.
func gcRoom(token string) {
	for {
		time.Sleep(5 * time.Minute)
		roomsM.Lock()
		r, ok := rooms[token]
		if !ok {
			roomsM.Unlock()
			return
		}
		if len(r.peers) == 0 && time.Since(r.created) > 10*time.Minute {
			delete(rooms, token)
			roomsM.Unlock()
			return
		}
		roomsM.Unlock()
	}
}

func dropPeer(r *room, c net.Conn, token string) {
	roomsM.Lock()
	delete(r.peers, c)
	// keep room alive briefly for reconnects; gcRoom reaps empty rooms later
	roomsM.Unlock()
	c.Close()
}

// writeFrame writes a control frame: 2-byte big-endian length prefix + JSON
// payload, matching the 2-byte length-prefix framing of readRegister.
func writeFrame(conn net.Conn, payload string) {
	header := []byte{byte(len(payload) >> 8), byte(len(payload) & 0xff)}
	conn.Write(append(header, payload...))
}

func handle(conn net.Conn) {
	defer func() {
		if rec := recover(); rec != nil {
			log.Printf("panic recovered: %v", rec)
		}
	}()

	// first frame must be REGISTER within 10s
	conn.SetReadDeadline(time.Now().Add(10 * time.Second))
	token, role, err := readRegister(conn)
	if err != nil {
		log.Printf("register failed from %s: %v", conn.RemoteAddr(), err)
		conn.Close()
		return
	}
	conn.SetReadDeadline(time.Time{}) // clear

	r, npeers, other, status := joinRoom(token, role, conn)
	switch status {
	case 1:
		log.Printf("room %s full, rejecting %s", token, conn.RemoteAddr())
		writeFrame(conn, `{"type":"ROOM_FULL"}`)
		conn.Close()
		return
	case 2:
		log.Printf("room %s already has a %s", token, role)
		writeFrame(conn, `{"type":"ROLE_TAKEN"}`)
		conn.Close()
		return
	}

	log.Printf("room %s: %s joined (%d/2) from %s", token, role, npeers, conn.RemoteAddr())

	if npeers == 2 && other != nil {
		notify := `{"type":"PEER_JOINED"}`
		other.Write([]byte(notify))
		conn.Write([]byte(notify))
	}

	// relay loop: forward everything to the other peer
	buf := make([]byte, 64*1024)
	for {
		n, err := conn.Read(buf)
		if n > 0 {
			roomsM.Lock()
			var dest net.Conn
			for peerConn := range r.peers {
				if peerConn != conn {
					dest = peerConn
				}
			}
			roomsM.Unlock()
			if dest != nil {
				if _, werr := dest.Write(buf[:n]); werr != nil {
					break
				}
			}
		}
		if err != nil {
			if err != io.EOF {
				log.Printf("room %s read error: %v", token, err)
			}
			break
		}
	}
	dropPeer(r, conn, token)
	log.Printf("room %s: %s left", token, role)
}

// readRegister parses a 2-byte length-prefixed JSON REGISTER frame.
func readRegister(conn net.Conn) (string, string, error) {
	header := make([]byte, 2)
	if _, err := io.ReadFull(conn, header); err != nil {
		return "", "", err
	}
	length := int(header[0])<<8 | int(header[1])
	if length > 4096 {
		return "", "", fmt.Errorf("register frame too large: %d", length)
	}
	payload := make([]byte, length)
	if _, err := io.ReadFull(conn, payload); err != nil {
		return "", "", err
	}

	// minimal JSON field extraction without pulling in encoding/json
	// (fields are simple ASCII: "room":"...","role":"...")
	s := string(payload)
	token := extractField(s, "room")
	role := extractField(s, "role")
	if token == "" || (role != "host" && role != "guest") {
		return "", "", fmt.Errorf("bad register payload: %s", s)
	}
	return token, role, nil
}

func extractField(s, field string) string {
	key := `"` + field + `":"`
	i := indexOf(s, key)
	if i < 0 {
		return ""
	}
	rest := s[i+len(key):]
	j := indexOf(rest, `"`)
	if j < 0 {
		return ""
	}
	return rest[:j]
}

func indexOf(s, sub string) int {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return i
		}
	}
	return -1
}

func main() {
	port := flag.Int("port", 25910, "TCP listen port")
	flag.Parse()

	ln, err := net.Listen("tcp", fmt.Sprintf(":%d", *port))
	if err != nil {
		log.Fatalf("listen: %v", err)
		os.Exit(1)
	}
	log.Printf("LiteKVM relay listening on :%d", *port)

	for {
		conn, err := ln.Accept()
		if err != nil {
			log.Printf("accept: %v", err)
			continue
		}
		go handle(conn)
	}
}
