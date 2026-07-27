@echo off
bcdedit /set testsigning on
bcdedit /enum {current} | findstr /i testsigning
