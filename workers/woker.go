package main

import (
	"fmt"
	"net"
)

func check(err error) {
	if err != nil {
		fmt.Println("error:", err.Error())
	}
}

func main() {
	listener, err := net.Listen("tcp", "localhost:8001")
	check(err)
	for {
		conn, err := listener.Accept()
		fmt.Println("accept client")
		check(err)
		go func(conn net.Conn) {
			/*reader := bufio.NewReader(conn)*/
			//head, err := reader.ReadString('\n')
			//head = head[1 : len(head)-2]
			//fmt.Println(head)
			//check(err)
			//size, err := strconv.Atoi(head[1:])
			//check(err)
			//var data = make([]byte, size)
			//str := io.LimitReader(reader, int64(size))
			//data, err = ioutil.ReadAll(str)
			/*check(err)*/
			var data = make([]byte, 1024)
			conn.Read(data)
			fmt.Println(string(data))
			conn.Close()
		}(conn)
	}
}
