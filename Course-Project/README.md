## Assumptions

### File/Dir that is being copied i.e., src should not have any name conflicts with the existing destination files/Dir.

### List all accesible paths functions prints all paths from the assumed root Directory. 

### Write format - WRITE PATH DATA ASYN_FlAG ->If flag is 1 forced write happens(Synchrouns) if it is 0 our SS decides if it has to be written Synchronously or Asynchronusly.Our Threshold for Asynchronous Write is 1MB of data.

### Our Copy src dest only works of when both are either Directoris or Files.

### All ports in Storage Server for Clients are unique.

### Copy will work on files if no client is Writing from the file at the same time.We have not handled cases where copy and write on a file are happening simultaneously. 

### We are not backing up streaming files(Music files).

### Every storage server is uniquely identified by naming server port and client port which are different.

### Backup won't work in case of write(i.e, when writing to a file), since we are not sending Acknowledgement to Naming server after a write.

### Time out message will be printed on client side if he doesnt get ACK within 5 sec after requesting but we are not terminating the request after 5 sec.