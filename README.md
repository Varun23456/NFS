
# Distributed Network File System (NFS) 

A distributed network file system implementation built in C as part of the Operating Systems and Networks course project. This system provides seamless file operations across multiple storage servers with fault tolerance, caching, and concurrent access management.


## Overview

This Network File System (NFS) implementation provides a distributed storage solution where multiple clients can access files stored across multiple storage servers through a centralized naming server. The system ensures data consistency, fault tolerance, and efficient file operations in a networked environment.

### Key Components
- **Naming Server (NS)**: Central coordinator managing metadata and storage server information
- **Storage Server (SS)**: Distributed storage nodes handling actual file storage and operations
- **Client (CL)**: Interface for users to perform file operations seamlessly


## Features

### Core Functionality
- **Distributed Storage**: Files distributed across multiple storage servers
- **Centralized Metadata**: Naming server maintains file system hierarchy
- **Transparent Access**: Clients access files as if they were local
- **Concurrent Operations**: Multiple clients can access the system simultaneously

### Advanced Features
- **Fault Tolerance**: Automatic failover and recovery mechanisms
- **Caching**: Client-side and server-side caching for improved performance
- **Load Balancing**: Intelligent distribution of files across storage servers
- **Consistency**: Strong consistency guarantees for file operations
- **Replication**: Data replication for reliability and availability

### File Operations
- **CREATE**: Create new files and directories
- **READ**: Read file contents with efficient streaming
- **WRITE**: Write and append to files
- **DELETE**: Remove files and directories
- **COPY**: Copy files within the system
- **LIST**: List directory contents
- **INFO**: Get file metadata and statistics

### System Operations
- **MOUNT**: Mount remote file systems
- **BACKUP**: Create backups of critical data
- **RESTORE**: Restore from backups

## Components

### Naming Server (NS)
Central coordinator that manages the file system namespace and metadata.

#### Responsibilities
- Maintain file system hierarchy and metadata
- Track storage server locations and availability
- Handle client requests and route to appropriate storage servers
- Manage file locking and consistency
- Coordinate backup and replication operations

#### Key Features
- **Metadata Management**: Efficient B-tree based metadata storage
- **Load Balancing**: Distribute files across storage servers
- **Fault Detection**: Monitor storage server health
- **Concurrency Control**: Handle concurrent client requests

### Storage Server (SS)
Distributed storage nodes that handle actual file storage and operations.

#### Responsibilities
- Store and manage file data
- Handle read/write operations
- Maintain local file system consistency
- Communicate with naming server for coordination
- Implement caching and buffering

#### Key Features
- **File Storage**: Efficient file storage and retrieval
- **Cache Management**: LRU-based caching system
- **Backup Support**: Participate in backup operations
- **Health Monitoring**: Report status to naming server

### Client (CL)
User interface for interacting with the distributed file system.

#### Responsibilities
- Provide user-friendly interface for file operations
- Handle client-side caching
- Manage connections to naming server
- Implement retry mechanisms for fault tolerance

#### Key Features
- **Command Interface**: Interactive shell for file operations
- **Batch Operations**: Support for scripted operations
- **Error Handling**: Comprehensive error reporting
- **Performance Monitoring**: Track operation statistics

## Installation

### Prerequisites
- GCC compiler (version 7.0 or higher)
- Linux/Unix environment
- Make utility
- Network connectivity between components

### Build Instructions
```bash
# Clone the repository
git clone https://github.com/yourusername/nfs-distributed.git
cd nfs-distributed

# Compile all components
make all

# Or compile individually
make ns    # Compile naming server
make ss    # Compile storage server
make cl    # Compile client
```

### Manual Compilation
```bash
# Compile Naming Server
gcc -o ns ns.c -lpthread -lm

# Compile Storage Server
gcc -o ss ss.c -lpthread -lm

# Compile Client
gcc -o cl cl.c -lpthread -lm
```

### Basic File Operations

#### File Management
```bash
# Create a file
CREATE /path/to/file.txt

# Write to file
WRITE /path/to/file.txt "Hello, NFS!"

# Read file contents
READ /path/to/file.txt

# Copy file
COPY /source/file.txt /destination/file.txt

# Delete file
DELETE /path/to/file.txt
```

#### Directory Operations
```bash
# Create directory
CREATE /path/to/directory

# List directory contents
LIST /path/to/directory

# Get file information
INFO /path/to/file.txt
```

#### Advanced Operations
```bash
# Mount remote file system
MOUNT /remote/path /local/mount/point

# Backup data
BACKUP /path/to/backup

# Synchronize cached data
SYNC
```

### Error Codes

| Code | Description |
|------|-------------|
| `0` | Success |
| `1` | File not found |
| `2` | Permission denied |
| `3` | Storage server unavailable |
| `4` | Network error |
| `5` | Invalid path |
| `6` | File already exists |
| `7` | Directory not empty |
| `8` | Insufficient space |
| `9` | Operation timeout |
| `10` | Unknown error |

## Project Structure

```
nfs-distributed/
├── ns.c                    # Naming Server implementation
├── ss.c                    # Storage Server implementation
├── cl.c                    # Client implementation
├── errors.h                # Error codes and handling
├── history.txt             # Command history storage
├── ns                      # Compiled naming server binary
├── ss                      # Compiled storage server binary
├── cl                      # Compiled client binary (if present)
├── README.md               # This file
├── makefile                # Build configuration
```

## Assumptions

### System Assumptions
- **Network Reliability**: Assumes reasonably stable network connectivity
- **Clock Synchronization**: All servers have synchronized clocks
- **File System**: Underlying file system supports required operations
- **Memory**: Sufficient memory available for caching and buffers

### Implementation Assumptions
- **Maximum File Size**: Files are limited to 2GB
- **Path Length**: Maximum path length is 4096 characters
- **Concurrent Clients**: Maximum 1000 concurrent client connections
- **Storage Servers**: Maximum 100 storage servers supported
- **File Names**: File names are case-sensitive and UTF-8 encoded

### Operational Assumptions
- **Backup Strategy**: Regular backups are performed by system administrators
- **Monitoring**: External monitoring systems track system health
- **Recovery**: Manual intervention may be required for catastrophic failures
- **Scaling**: Horizontal scaling requires manual configuration

### Error Handling Assumptions
- **Timeout Values**: Default timeout values may need adjustment based on network conditions
- **Retry Logic**: Exponential backoff is used for retry operations
- **Logging**: All errors are logged with appropriate severity levels
- **Graceful Degradation**: System continues operating with reduced functionality during failures


## Known Issues and Limitations

### Current Limitations
- **Single Point of Failure**: Naming server represents a single point of failure
- **Limited Scalability**: Performance degrades with very large numbers of files
- **Cache Consistency**: Some cache consistency issues under high concurrency
- **Recovery Time**: Recovery from failures may take significant time

### Future Improvements
- **Distributed Naming Server**: Implement distributed naming server for high availability
- **Advanced Caching**: Implement more sophisticated caching strategies
- **Compression**: Add file compression support
- **Encryption**: Implement end-to-end encryption
- **Monitoring Dashboard**: Web-based monitoring and administration interface

