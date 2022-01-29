#!/usr/bin/env python3

import os
import sys
import socket
import subprocess
import signal
import re

port = 2001
wait_for_process=True

def get_data_ascii(conn):
    data=conn.recv(4096)
    return data.decode('ascii')


def signal_handler(sig, frame):
    global wait_for_process
    print('Got siglal, returning status to client')
    wait_for_process=False

signal.signal(signal.SIGUSR1, signal_handler)

def execute(command):
    global wait_for_process
    # Change "$$" with my PID
    command=re.sub('\$\$', str(os.getpid()), command)
    p = subprocess.Popen(command.split())
    while wait_for_process:
        try:
            p.wait(1)
            break
        except subprocess.TimeoutExpired as e:
            pass
    retval = p.returncode
    if retval is None:
        retval=0 
    print('Returning retval %d' % retval, flush=True)
    return retval


def send(ip, data):
    try:
        print('Connecting to \"%s\"...' % ip, flush=True)
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect((ip, port))
            print('Connected; sending \"%s\"...' % data, flush=True)
            data = bytes(data, 'ascii')
            s.sendall(data)
            print('Waiting for the server to finish executing the command...')
            if get_data_ascii(s) == 'done':
                exit(0)
            else:
                print('Error on server side')
                exit(1)
    except ConnectionRefusedError:
        print('Server has not yet started!')
        exit(1)


def start_server():
    print('Starting server, listening on port %d' % port)
    running = True
    s=socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(('0.0.0.0', port))
    s.listen()
    try:
        while running:
            conn, addr = s.accept()
            try:
                data = get_data_ascii(conn)
                print('Got \"%s\"' % data, flush=True)
                if data == 'exit':
                    running = False
                else:
                    retval = execute(data)
                if retval == 0:
                    print('Done')
                    conn.sendall(b'done')
                else:
                    print('Error')
                    conn.sendall(b'error')
                print('Waiting for the next connection')
            except KeyboardInterrupt as e:
                running = False
            conn.close()
    except KeyboardInterrupt as e:
        pass
    s.close()
    print('Exiting server')


if __name__ == '__main__':
    argc = len(sys.argv)
    if argc <= 1:
        print('Usage: %s start-server|send [ip] [command[command]]'
              % sys.argv[0])
        print(' * ip: IP of server to send command to')
        print(' * command: system command to send to the server')
        exit(1)
    if sys.argv[1] == 'start-server':
        start_server()
    elif sys.argv[1] == 'send' and argc < 4:
        print('Missing IP and/or commands to send')
        exit(1)
    elif sys.argv[1] == 'send' and argc >= 4:    
        ip = sys.argv[2]
        command = ' '.join(sys.argv[3:])
        send(ip, command)

