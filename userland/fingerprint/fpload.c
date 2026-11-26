// fpload: hand files to the kernel's Goodix file store (kern.sweet_qsee_file = "name\0bytes")
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <sys/sysctl.h>

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		FILE *f = fopen(argv[i], "rb");
		if (!f) { perror(argv[i]); return 1; }
		if (fseek(f, 0, SEEK_END) != 0) { perror("fseek"); fclose(f); return 1; }
		long n = ftell(f);
		if (n < 0) { perror("ftell"); fclose(f); return 1; }
		if (fseek(f, 0, SEEK_SET) != 0) { perror("fseek"); fclose(f); return 1; }
		const char *name = basename(argv[i]);
		size_t nl = strlen(name) + 1;
		char *buf = malloc(nl + (size_t)n);
		if (!buf) { perror("malloc"); fclose(f); return 1; }
		memcpy(buf, name, nl);
		if (n > 0 && fread(buf + nl, 1, (size_t)n, f) != (size_t)n) {
			perror("read");
			free(buf);
			fclose(f);
			return 1;
		}
		fclose(f);
		int r = sysctlbyname("kern.sweet_qsee_file", NULL, NULL, buf, nl + (size_t)n);
		printf("%s: %ld bytes -> %s\n", name, n, r == 0 ? "ok" : "failed");
		if (r != 0) perror("sysctl");
		free(buf);
	}
	char out[4096]; size_t ol = sizeof(out) - 1;
	if (sysctlbyname("kern.sweet_qsee_files", out, &ol, NULL, 0) == 0) { out[ol] = 0; printf("%s\n", out); }
	return 0;
}
