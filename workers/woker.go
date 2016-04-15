package main

import (
	"fmt"
	"net"
)

func check(err error) {
	if err != nil {
		fmt.Println("error: %s", err.Error())
	}
}

func main() {
	listener, err := net.Listen("tcp", "localhost:8001")
	check(err)
	for {
		conn, err := listener.Accept()
		fmt.Println("accept clients")
		check(err)
		go func(conn net.Conn) {
			buf := make([]byte, 1024)
			_, err := conn.Read(buf)
			check(err)
			fmt.Println(string(buf))
			conn.Close()
		}(conn)
	}
}
