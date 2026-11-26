/*
 * SapphireBiometry.dylib — "Use Fingerprint / Face ID for Apps and Purchases" (Huella / fpd / faced)
 *
 * Hooks LocalAuthentication's LAContext across UIKit apps & App Store:
 * - Reads /var/mobile/Library/Preferences/com.sapphire.fingerprint.plist ("appsEnabled")
 *   and /var/mobile/Library/Preferences/com.sapphire.faceid.plist ("faceAppsEnabled").
 * - Reports LABiometryTypeTouchID (1) / LABiometryTypeFaceID (2) and canEvaluatePolicy:YES
 *   when enabled & enrolled.
 * - On evaluatePolicy:localizedReason:reply:, displays a native iOS auth card +
 *   optical FOD target ring at the exact hardware sensor position (540/1080, 2109/2400),
 *   authenticates via /var/run/fpd.sock ("TEST") and/or /var/run/faced.sock ("SCAN_BIO"),
 *   triggers haptics, and returns reply(YES, nil) on match.
 */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <notify.h>

#define FPD_SOCK_PATH   "/var/run/fpd.sock"
#define FACED_SOCK_PATH "/var/run/faced.sock"
#define FP_PREFS_PATH   @"/var/mobile/Library/Preferences/com.sapphire.fingerprint.plist"
#define FACE_PREFS_PATH @"/var/mobile/Library/Preferences/com.sapphire.faceid.plist"
#define SENSOR_X        (540.0 / 1080.0)
#define SENSOR_Y        (2109.0 / 2400.0)
#define SENSOR_R        (105.0 / 1080.0)

static BOOL
bio_apps_enabled(void)
{
	NSDictionary *d = [NSDictionary dictionaryWithContentsOfFile:FP_PREFS_PATH];
	if (d == nil) return YES; /* default ON once installed */
	id v = d[@"appsEnabled"];
	return v ? [v boolValue] : YES;
}

static BOOL
face_apps_enabled(void)
{
	NSDictionary *d = [NSDictionary dictionaryWithContentsOfFile:FACE_PREFS_PATH];
	if (d == nil) return YES;
	id v = d[@"faceAppsEnabled"];
	return v ? [v boolValue] : YES;
}

static void
faced_send_stop_async(void)
{
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		int fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0) return;
		int one = 1;
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
		struct timeval tv = { 1, 0 };
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		struct sockaddr_un sa;
		memset(&sa, 0, sizeof(sa));
		sa.sun_family = AF_UNIX;
		strlcpy(sa.sun_path, FACED_SOCK_PATH, sizeof(sa.sun_path));
		if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
			const char *cmd = "STOP\n";
			if (write(fd, cmd, strlen(cmd)) == (ssize_t)strlen(cmd)) {
				char rep[32];
				(void)read(fd, rep, sizeof(rep));
			}
		}
		close(fd);
	});
}

static BOOL
faced_has_enrolled_face(void)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return NO;
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	struct timeval tv = { 1, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	struct sockaddr_un a;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strlcpy(a.sun_path, FACED_SOCK_PATH, sizeof(a.sun_path));
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		close(fd);
		return NO;
	}
	const char *cmd = "STATUS\n";
	BOOL ok = NO;
	if (write(fd, cmd, strlen(cmd)) == (ssize_t)strlen(cmd)) {
		char buf[512];
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = '\0';
			const char *f = strstr(buf, "enrolled=");
			if (f && atoi(f + 9) > 0) ok = YES;
		}
	}
	close(fd);
	return ok;
}

static BOOL
fpd_has_enrolled_fingers(void)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return NO;
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	struct timeval tv = { 1, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	struct sockaddr_un a;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strlcpy(a.sun_path, FPD_SOCK_PATH, sizeof(a.sun_path));
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		close(fd);
		return NO;
	}
	const char *cmd = "STATUS\n";
	BOOL ok = NO;
	if (write(fd, cmd, strlen(cmd)) == (ssize_t)strlen(cmd)) {
		char buf[512];
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = '\0';
			const char *f = strstr(buf, "enrolled=");
			const char *fg = strstr(buf, "fingers=");
			if ((fg && atoi(fg + 8) > 0) || (f && atoi(f + 9) > 0)) ok = YES;
		}
	}
	close(fd);
	return ok;
}

static BOOL
bio_any_enrolled_and_enabled(void)
{
	if (bio_apps_enabled() && fpd_has_enrolled_fingers()) return YES;
	if (face_apps_enabled() && faced_has_enrolled_face()) return YES;
	return NO;
}

static UIWindow *
bio_key_window(void)
{
	UIApplication *app = [UIApplication sharedApplication];
	for (UIScene *s in app.connectedScenes) {
		if (![s isKindOfClass:[UIWindowScene class]]) continue;
		UIWindowScene *ws = (UIWindowScene *)s;
		for (UIWindow *w in ws.windows) {
			if (w.isKeyWindow) return w;
		}
		if (ws.windows.firstObject) return ws.windows.firstObject;
	}
	return nil;
}

@interface SapphireBioPrompt : UIView
@property (nonatomic, copy) void (^replyBlock)(BOOL success, NSError *error);
@property (nonatomic, strong) UILabel *titleLabel;
@property (nonatomic, strong) UILabel *reasonLabel;
@property (nonatomic, strong) UILabel *statusLabel;
@property (nonatomic, strong) UIView *sensorSpot;
@property (nonatomic, assign) int sockFd;
@property (nonatomic, assign) int faceSockFd;
@property (nonatomic, assign) BOOL hasFace;
@property (nonatomic, assign) BOOL hasFP;
@property (nonatomic, assign) BOOL finished;
@property (nonatomic, assign) int attempts;
@property (nonatomic, assign) int faceAttempts;
@end

@implementation SapphireBioPrompt

- (instancetype)initWithFrame:(CGRect)frame reason:(NSString *)reason reply:(void (^)(BOOL, NSError *))reply
{
	self = [super initWithFrame:frame];
	if (!self) return nil;
	_replyBlock = [reply copy];
	_sockFd = -1;
	_faceSockFd = -1;
	_finished = NO;
	_attempts = 0;
	_faceAttempts = 0;
	self.backgroundColor = [[UIColor blackColor] colorWithAlphaComponent:0.55];

	CGFloat w = frame.size.width;
	CGFloat h = frame.size.height;

	/* Centered iOS-style frosted card */
	UIBlurEffect *blur = [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemChromeMaterialDark];
	UIVisualEffectView *card = [[UIVisualEffectView alloc] initWithEffect:blur];
	card.frame = CGRectMake(28, h * 0.30, w - 56, 210);
	card.layer.cornerRadius = 22;
	card.clipsToBounds = YES;
	[self addSubview:card];

	BOOL hasFace = (face_apps_enabled() && faced_has_enrolled_face());
	BOOL hasFP = (bio_apps_enabled() && fpd_has_enrolled_fingers());
	_hasFace = hasFace;
	_hasFP = hasFP;
	NSString *glyphName = (hasFace && !hasFP) ? @"faceid" : (hasFace ? @"person.badge.shield.checkmark" : @"touchid");

	UIImageView *icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:glyphName]];
	icon.tintColor = [UIColor systemBlueColor];
	icon.contentMode = UIViewContentModeScaleAspectFit;
	icon.frame = CGRectMake((card.bounds.size.width - 44) / 2.0, 18, 44, 44);
	[card.contentView addSubview:icon];

	_titleLabel = [[UILabel alloc] initWithFrame:CGRectMake(16, 68, card.bounds.size.width - 32, 24)];
	_titleLabel.text = (hasFace && !hasFP) ? @"Face ID (Huella)" : (hasFace ? @"Face ID / Touch ID" : @"Touch ID (Huella)");
	_titleLabel.textColor = [UIColor whiteColor];
	_titleLabel.font = [UIFont boldSystemFontOfSize:17];
	_titleLabel.textAlignment = NSTextAlignmentCenter;
	[card.contentView addSubview:_titleLabel];

	_reasonLabel = [[UILabel alloc] initWithFrame:CGRectMake(16, 94, card.bounds.size.width - 32, 38)];
	_reasonLabel.text = reason.length ? reason : (hasFace ? (hasFP ? @"Look at front camera or use fingerprint" : @"Authenticate with Face ID") : @"Authenticate with your fingerprint");
	_reasonLabel.textColor = [[UIColor whiteColor] colorWithAlphaComponent:0.85];
	_reasonLabel.font = [UIFont systemFontOfSize:13];
	_reasonLabel.textAlignment = NSTextAlignmentCenter;
	_reasonLabel.numberOfLines = 2;
	[card.contentView addSubview:_reasonLabel];

	_statusLabel = [[UILabel alloc] initWithFrame:CGRectMake(16, 134, card.bounds.size.width - 32, 20)];
	_statusLabel.text = hasFace ? (hasFP ? @"Scanning face / Touch sensor" : @"Scanning face…") : @"Place finger on sensor";
	_statusLabel.textColor = [UIColor systemTealColor];
	_statusLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
	_statusLabel.textAlignment = NSTextAlignmentCenter;
	[card.contentView addSubview:_statusLabel];

	UIButton *cancel = [UIButton buttonWithType:UIButtonTypeSystem];
	cancel.frame = CGRectMake(0, 162, card.bounds.size.width, 46);
	[cancel setTitle:@"Cancel" forState:UIControlStateNormal];
	[cancel setTitleColor:[UIColor systemBlueColor] forState:UIControlStateNormal];
	cancel.titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
	[cancel addTarget:self action:@selector(cancelTapped) forControlEvents:UIControlEventTouchUpInside];
	[card.contentView addSubview:cancel];

	/* Optical FOD sensor target ring at exact hardware position */
	if (hasFP) {
		CGFloat sx = w * SENSOR_X;
		CGFloat sy = h * SENSOR_Y;
		CGFloat sr = w * SENSOR_R;
		_sensorSpot = [[UIView alloc] initWithFrame:CGRectMake(sx - sr, sy - sr, sr * 2.0, sr * 2.0)];
		_sensorSpot.layer.cornerRadius = sr;
		_sensorSpot.layer.borderWidth = 3.0;
		_sensorSpot.layer.borderColor = [UIColor systemGreenColor].CGColor;
		_sensorSpot.backgroundColor = [[UIColor whiteColor] colorWithAlphaComponent:0.18];
		UIImageView *fp = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"touchid"]];
		fp.tintColor = [UIColor whiteColor];
		fp.contentMode = UIViewContentModeScaleAspectFit;
		fp.frame = CGRectInset(_sensorSpot.bounds, 14, 14);
		[_sensorSpot addSubview:fp];
		[self addSubview:_sensorSpot];
	}

	[self startFaceScan];
	if (hasFP) {
		[self startTest];
	} else if (!hasFace) {
		[self finishWithSuccess:NO code:-7];
	}
	return self;
}

/*
 * Unbinds active socket FDs and wakes any blocked reader threads via shutdown().
 * Each reader thread remains the sole owner of close(fd) to prevent FD-reuse races.
 */
- (void)closeSock:(BOOL)sendCancel
{
	int fpFd = -1;
	int faceFd = -1;
	@synchronized(self) {
		fpFd = _sockFd;
		_sockFd = -1;
		faceFd = _faceSockFd;
		_faceSockFd = -1;
	}
	if (fpFd >= 0) {
		if (sendCancel) {
			const char *c = "TEST_CANCEL\n";
			(void)write(fpFd, c, strlen(c));
		}
		shutdown(fpFd, SHUT_RDWR);
	}
	if (faceFd >= 0) {
		if (sendCancel) {
			/* Send out-of-band STOP over a fresh connection so faced aborts active scan */
			faced_send_stop_async();
		}
		shutdown(faceFd, SHUT_RDWR);
	}
}

- (void)finishWithSuccess:(BOOL)ok code:(NSInteger)errCode
{
	void (^cb)(BOOL, NSError *) = nil;
	@synchronized(self) {
		if (_finished) return;
		_finished = YES;
		cb = _replyBlock;
		_replyBlock = nil;
	}
	[self closeSock:!ok];
	dispatch_async(dispatch_get_main_queue(), ^{
		[UIView animateWithDuration:0.18 animations:^{
			self.alpha = 0.0;
		} completion:^(BOOL d) {
			(void)d;
			[self removeFromSuperview];
			if (cb) {
				NSError *err = ok ? nil : [NSError errorWithDomain:@"com.apple.LocalAuthentication" code:errCode userInfo:nil];
				cb(ok, err);
			}
		}];
	});
}

- (void)cancelTapped
{
	[self finishWithSuccess:NO code:-2]; /* LAErrorUserCancel */
}

- (BOOL)isFinishedSync
{
	@synchronized(self) {
		return _finished;
	}
}

- (void)startFaceScan
{
	if ([self isFinishedSync] || !_hasFace) return;
	__weak typeof(self) ws = self;
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		__strong typeof(ws) s = ws;
		if (!s || [s isFinishedSync]) return;

		int fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0) {
			if (!s.hasFP) [s finishWithSuccess:NO code:-1];
			return;
		}
		int one = 1;
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
		struct timeval tv = { 5, 0 };
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		struct sockaddr_un sa;
		memset(&sa, 0, sizeof(sa));
		sa.sun_family = AF_UNIX;
		strlcpy(sa.sun_path, FACED_SOCK_PATH, sizeof(sa.sun_path));
		if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
			close(fd);
			if (!s.hasFP) [s finishWithSuccess:NO code:-7];
			return;
		}

		BOOL abortNow = NO;
		@synchronized(s) {
			if (s.finished) abortNow = YES;
			else s.faceSockFd = fd;
		}
		if (abortNow) {
			close(fd);
			return;
		}

		const char *cmd = "SCAN_BIO\n";
		ssize_t wn = write(fd, cmd, strlen(cmd));
		char buf[256] = "";
		ssize_t n = (wn == (ssize_t)strlen(cmd)) ? read(fd, buf, sizeof(buf) - 1) : -1;

		@synchronized(s) {
			if (s.faceSockFd == fd) s.faceSockFd = -1;
		}
		close(fd);

		if ([s isFinishedSync]) return;

		if (n > 0) {
			buf[n] = '\0';
			if (strncmp(buf, "MATCH", 5) == 0) {
				notify_post("com.sapphire.vib.success");
				dispatch_async(dispatch_get_main_queue(), ^{
					s.statusLabel.text = @"Face Verified";
					s.statusLabel.textColor = [UIColor systemGreenColor];
				});
				[s finishWithSuccess:YES code:0];
				return;
			}
			if (strncmp(buf, "CANCELLED", 9) == 0) {
				if (!s.hasFP && ![s isFinishedSync]) {
					[s finishWithSuccess:NO code:-2];
				}
				return;
			}
			BOOL isNoMatch = (strncmp(buf, "NOMATCH", 7) == 0);
			BOOL isNoFace = (strncmp(buf, "NOFACE", 6) == 0);
			BOOL isBusy = (strncmp(buf, "BUSY", 4) == 0);
			if (isNoMatch) {
				notify_post("com.sapphire.vib.error");
			}
			if (!s.hasFP) {
				int tries = 0;
				@synchronized(s) {
					tries = ++s.faceAttempts;
				}
				if ((isNoMatch || isNoFace || isBusy) && tries < 3) {
					NSString *msg = isNoMatch ? @"Face Not Recognized — Trying again…"
					    : (isBusy ? @"Camera busy — Retrying…" : @"Position face in view…");
					dispatch_async(dispatch_get_main_queue(), ^{
						if (![s isFinishedSync]) {
							s.statusLabel.text = msg;
							s.statusLabel.textColor = [UIColor systemOrangeColor];
						}
					});
					dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(450 * NSEC_PER_MSEC)),
					    dispatch_get_main_queue(), ^{
						[ws startFaceScan];
					});
				} else {
					dispatch_async(dispatch_get_main_queue(), ^{
						s.statusLabel.text = isNoMatch ? @"Face Not Recognized" : @"Face ID Failed";
						s.statusLabel.textColor = [UIColor systemRedColor];
					});
					[s finishWithSuccess:NO code:-1];
				}
			} else {
				dispatch_async(dispatch_get_main_queue(), ^{
					if (![s isFinishedSync]) {
						s.statusLabel.text = isNoMatch ? @"Face not recognized — Touch sensor" : @"Place finger on sensor";
						s.statusLabel.textColor = isNoMatch ? [UIColor systemOrangeColor] : [UIColor systemTealColor];
					}
				});
			}
			return;
		}

		/* Read failed or daemon closed socket */
		if (!s.hasFP) {
			[s finishWithSuccess:NO code:-1];
		}
	});
}

- (void)startTest
{
	if ([self isFinishedSync] || !_hasFP) return;

	__weak typeof(self) ws = self;
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		__strong typeof(ws) sObj = ws;
		if (!sObj || [sObj isFinishedSync]) return;

		int fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0) {
			if (!sObj.hasFace) [sObj finishWithSuccess:NO code:-1];
			return;
		}
		int one = 1;
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
		struct sockaddr_un a;
		memset(&a, 0, sizeof(a));
		a.sun_family = AF_UNIX;
		strlcpy(a.sun_path, FPD_SOCK_PATH, sizeof(a.sun_path));
		if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
			close(fd);
			if (!sObj.hasFace) [sObj finishWithSuccess:NO code:-7];
			return;
		}

		BOOL abortNow = NO;
		@synchronized(sObj) {
			if (sObj.finished) abortNow = YES;
			else sObj.sockFd = fd;
		}
		if (abortNow) {
			close(fd);
			return;
		}

		const char *cmd = "TEST\n";
		if (write(fd, cmd, strlen(cmd)) != (ssize_t)strlen(cmd)) {
			@synchronized(sObj) {
				if (sObj.sockFd == fd) sObj.sockFd = -1;
			}
			close(fd);
			if (!sObj.hasFace) [sObj finishWithSuccess:NO code:-1];
			return;
		}

		NSMutableData *buf = [NSMutableData data];
		char tmp[512];
		BOOL retryTest = NO;
		BOOL matched = NO;
		BOOL failed = NO;

		for (;;) {
			BOOL active = NO;
			@synchronized(sObj) {
				active = (!sObj.finished && sObj.sockFd == fd);
			}
			if (!active) break;

			ssize_t n = read(fd, tmp, sizeof(tmp));
			if (n <= 0) break;
			[buf appendBytes:tmp length:(NSUInteger)n];

			BOOL doneLoop = NO;
			for (;;) {
				const char *raw = buf.bytes;
				const char *nl = memchr(raw, '\n', buf.length);
				if (!nl) break;
				NSString *line = [[NSString alloc] initWithBytes:raw length:(NSUInteger)(nl - raw) encoding:NSUTF8StringEncoding];
				[buf replaceBytesInRange:NSMakeRange(0, (NSUInteger)(nl - raw) + 1) withBytes:NULL length:0];
				if (!line) continue;
				if ([line isEqualToString:@"PRESS"]) {
					dispatch_async(dispatch_get_main_queue(), ^{
						ws.statusLabel.text = @"Verifying Fingerprint…";
						ws.sensorSpot.backgroundColor = [UIColor whiteColor];
					});
				} else if ([line hasPrefix:@"TEST MATCH"]) {
					matched = YES;
					doneLoop = YES;
					break;
				} else if ([line hasPrefix:@"TEST NOMATCH"] || [line hasPrefix:@"TEST LIFTED"]) {
					notify_post("com.sapphire.vib.error");
					int tries = 0;
					@synchronized(sObj) {
						tries = ++sObj.attempts;
					}
					BOOL lifted = [line hasPrefix:@"TEST LIFTED"];
					dispatch_async(dispatch_get_main_queue(), ^{
						ws.sensorSpot.backgroundColor = [[UIColor whiteColor] colorWithAlphaComponent:0.18];
						ws.statusLabel.text = lifted ? @"Hold finger slightly longer" : @"Try again";
						ws.statusLabel.textColor = [UIColor systemOrangeColor];
					});
					if (tries >= 5) {
						failed = YES;
					} else {
						retryTest = YES;
					}
					doneLoop = YES;
					break;
				} else if ([line hasPrefix:@"TEST TIMEOUT"] || [line hasPrefix:@"ERR"]) {
					failed = YES;
					doneLoop = YES;
					break;
				}
			}
			if (doneLoop) break;
		}

		@synchronized(sObj) {
			if (sObj.sockFd == fd) sObj.sockFd = -1;
		}
		close(fd);

		if (matched) {
			notify_post("com.sapphire.vib.success");
			dispatch_async(dispatch_get_main_queue(), ^{
				ws.statusLabel.text = @"Recognized";
				ws.statusLabel.textColor = [UIColor systemGreenColor];
			});
			[sObj finishWithSuccess:YES code:0];
		} else if (retryTest && ![sObj isFinishedSync]) {
			dispatch_async(dispatch_get_main_queue(), ^{
				[ws startTest];
			});
		} else if (failed && ![sObj isFinishedSync]) {
			[sObj finishWithSuccess:NO code:-1];
		}
	});
}
@end

/* ================================================================== LAContext hooks */
static BOOL (*orig_can_eval)(id self, SEL sel, NSInteger policy, NSError **error);
static BOOL
hook_can_eval(id self, SEL sel, NSInteger policy, NSError **error)
{
	if ((policy == 1 || policy == 2) && bio_any_enrolled_and_enabled()) {
		if (error) *error = nil;
		return YES;
	}
	return orig_can_eval ? orig_can_eval(self, sel, policy, error) : NO;
}

static NSInteger (*orig_bio_type)(id self, SEL sel);
static NSInteger
hook_bio_type(id self, SEL sel)
{
	if (bio_any_enrolled_and_enabled()) {
		if (face_apps_enabled() && faced_has_enrolled_face() && (!bio_apps_enabled() || !fpd_has_enrolled_fingers())) {
			return 2; /* LABiometryTypeFaceID */
		}
		return 1; /* LABiometryTypeTouchID */
	}
	return orig_bio_type ? orig_bio_type(self, sel) : 0;
}

static NSData *(*orig_domain_state)(id self, SEL sel);
static NSData *
hook_domain_state(id self, SEL sel)
{
	if (bio_any_enrolled_and_enabled()) {
		static NSData *tok = nil;
		if (!tok) tok = [@"sapphire-bio-v3" dataUsingEncoding:NSUTF8StringEncoding];
		return tok;
	}
	return orig_domain_state ? orig_domain_state(self, sel) : nil;
}

static void (*orig_eval_policy)(id self, SEL sel, NSInteger policy, NSString *reason, void (^reply)(BOOL, NSError *));
static void
hook_eval_policy(id self, SEL sel, NSInteger policy, NSString *reason, void (^reply)(BOOL, NSError *))
{
	if ((policy == 1 || policy == 2) && bio_any_enrolled_and_enabled()) {
		dispatch_async(dispatch_get_main_queue(), ^{
			UIWindow *w = bio_key_window();
			if (!w) {
				if (orig_eval_policy) orig_eval_policy(self, sel, policy, reason, reply);
				else if (reply) reply(NO, [NSError errorWithDomain:@"com.apple.LocalAuthentication" code:-1 userInfo:nil]);
				return;
			}
			SapphireBioPrompt *p = [[SapphireBioPrompt alloc] initWithFrame:w.bounds reason:reason reply:reply];
			p.alpha = 0.0;
			[w addSubview:p];
			[UIView animateWithDuration:0.18 animations:^{ p.alpha = 1.0; }];
		});
		return;
	}
	if (orig_eval_policy) orig_eval_policy(self, sel, policy, reason, reply);
}

__attribute__((constructor)) static void
sapphire_biometry_init(void)
{
	Class la = objc_getClass("LAContext");
	if (la == Nil) return;

	Method m1 = class_getInstanceMethod(la, sel_registerName("canEvaluatePolicy:error:"));
	if (m1) {
		orig_can_eval = (BOOL (*)(id, SEL, NSInteger, NSError **))method_getImplementation(m1);
		method_setImplementation(m1, (IMP)hook_can_eval);
	}
	Method m2 = class_getInstanceMethod(la, sel_registerName("biometryType"));
	if (m2) {
		orig_bio_type = (NSInteger (*)(id, SEL))method_getImplementation(m2);
		method_setImplementation(m2, (IMP)hook_bio_type);
	}
	Method m3 = class_getInstanceMethod(la, sel_registerName("evaluatedPolicyDomainState"));
	if (m3) {
		orig_domain_state = (NSData *(*)(id, SEL))method_getImplementation(m3);
		method_setImplementation(m3, (IMP)hook_domain_state);
	}
	Method m4 = class_getInstanceMethod(la, sel_registerName("evaluatePolicy:localizedReason:reply:"));
	if (m4) {
		orig_eval_policy = (void (*)(id, SEL, NSInteger, NSString *, void (^)(BOOL, NSError *)))method_getImplementation(m4);
		method_setImplementation(m4, (IMP)hook_eval_policy);
	}
}
