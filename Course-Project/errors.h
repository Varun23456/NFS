/* File System Errors  */
#define E_NOFILE    -1     // File not found
#define E_CLIENT    -2     // Client error
#define E_DOPEN     -3     // Directory open error
#define E_NODIR     -4     // Directory not found
#define E_FOPEN     -6     // File open error
#define E_FTYPE     -8     // File type error
#define E_EPATH     -28    // Invalid path
#define E_FCREATE   -46    // File create error
#define E_DIRCREATE -47    // Dir create error


/* Network Related Errors */
#define E_RECV      -7     // Receive error
#define E_IP        -9     // IP error
#define E_CLOSE     -10    // Close error
#define E_WRITE     -11    // Write error
#define E_READ      -12    // Read error
#define E_CONN      -13    // Connect error
#define E_ACPT      -14    // Accept error
#define E_LSTN      -15    // Listen error
#define E_BIND      -16    // Bind error
#define E_GETNAME   -18    // Get name info error
#define E_SOCK      -17    // Socket creation error
#define E_SEND      -25    // Send error
#define E_INACTIVE  -29    // Server inactive
#define E_SOCKNAME  -30    // Socket name error
#define E_NOADDR    -42    // Invalid address
#define E_SOCKOPT   -44    // Setsockopt error
#define E_NTOP      -45    // inet_ntop error
#define E_GETIF     -46    // getifaddrs error

/* User Command Errors */
#define E_CMD       -19    // Invalid command
#define E_SYNTAX    -26    // Command syntax error
#define E_INVAL     -27    // Invalid input

/* Success Code */
#define E_SUCCESS    0     // Operation successful

/* Memory */
#define E_NULL      -31    // Null pointer error
#define E_NOMEM     -32    // Failed to allocate memory
#define E_BOUNDS    -33    // Array/Buffer bounds error
#define E_FREE      -34    // Invalid free/double free
#define E_OVERFLOW  -37    // Buffer overflow
#define E_UNDERFLOW -38    // Buffer underflow
#define E_LEAK      -40    // Memory leak detected
#define E_MMAP      -43    // Memory mapping error