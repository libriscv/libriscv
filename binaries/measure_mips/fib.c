static long fib(long n, long acc, long prev)
{
    if (n < 1)
        return acc;
    else
        return fib(n - 1, prev + acc, acc);
}

int main(int argc, char** argv)
{
    const volatile long n = 256000000;
    return fib(n, 0, 1);
}
