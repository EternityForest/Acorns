class _Acorns
{

  public:
    void replChar(char);
    void begin();
};


extern _Acorns Acorns;




//How many threads in the thread pool
#define  ACORNS_THREADS 4

//How many slots in the process table
#define ACORNS_MAXPROGRAMS 16