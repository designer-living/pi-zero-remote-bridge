To compile:

```
gcc -O3 -march=armv8-a -mtune=cortex-a53 -o remote_bridge remote_bridge.c
```

To run:
```
sudo ./remote_bridge "<Remote name from evtest>" <server ip> <server port>
```
