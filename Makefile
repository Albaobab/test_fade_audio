LDFLAGS := -lasound -lncurses
CFLAGS := -O0 -g


default : fade_audio

clean :
	rm -f *.o
	rm -f fade_audio

fade_audio : fade_audio.o
	gcc -o $@ $^ $(LDFLAGS)

./%.o : ./%.c
	gcc $(CFLAGS) -c -o $@ $<
