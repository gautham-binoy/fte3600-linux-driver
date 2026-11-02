savedcmd_fte3600.mod := printf '%s\n'   fte3600.o | awk '!x[$$0]++ { print("./"$$0) }' > fte3600.mod
