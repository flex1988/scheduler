## REDIS TASK
redis task is a schedule task control program based on redis.

### START SERVER

./server --daemonize or ./server will run direct

### COMMAND

#### RPC MESSAGE NOTIFY

notify wokers via tcp protocol serialize user message in RESP.

1. rpc once 1000 localhost:8001 {message}

    return timeId
    every timeEvent will has a timeId
    you can delete it use del command
    during server run time every timeEvent will has unique timeId

2. rpc repeat 1000 localhost:8001 {message}

#### DEL

1. del timeId

    +ok

### TODO

1. info command
2. incr command
3. decr command
4. aof persisent
5. use skiplist to promote performance
