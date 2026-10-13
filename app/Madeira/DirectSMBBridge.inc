/*
 * DirectSMBBridge.inc
 *
 * Direct SMB2/SMB3 access for Madeira. This bypasses UIDocumentPicker/File Provider.
 * The remote share remains authoritative. Madeira builds only a sparse metadata mirror.
 * Normal Wine reads are fetched range-by-range by hooks in ntdll. mmap-backed files are
 * materialized individually because iOS cannot expose a userspace SMB client as a kernel
 * filesystem/page-fault provider without a privileged filesystem extension.
 */

@import UIKit;
#import <Foundation/Foundation.h>
#import <os/log.h>
#import "WineProcessBridge.h"
#import "WineServerBridge.h"

#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>

struct smb2_context;
struct smb2fh;
struct smb2dir;

#define SMB2_TYPE_FILE      0x00000000
#define SMB2_TYPE_DIRECTORY 0x00000001
#define SMB2_TYPE_LINK      0x00000002
#define SMB2_SEC_NTLMSSP    1

struct smb2_stat_64 {
    uint32_t smb2_type;
    uint32_t smb2_nlink;
    uint64_t smb2_ino;
    uint64_t smb2_size;
    uint64_t smb2_atime;
    uint64_t smb2_atime_nsec;
    uint64_t smb2_mtime;
    uint64_t smb2_mtime_nsec;
    uint64_t smb2_ctime;
    uint64_t smb2_ctime_nsec;
    uint64_t smb2_btime;
    uint64_t smb2_btime_nsec;
    uint32_t smb2_attributes;
    uint32_t smb2_reparse_tag;
};
struct smb2dirent {
    const char *name;
    struct smb2_stat_64 st;
};

extern struct smb2_context *smb2_init_context(void);
extern void smb2_destroy_context(struct smb2_context *);
extern void smb2_set_user(struct smb2_context *, const char *);
extern void smb2_set_password(struct smb2_context *, const char *);
extern void smb2_set_domain(struct smb2_context *, const char *);
extern void smb2_set_authentication(struct smb2_context *, int);
extern void smb2_set_timeout(struct smb2_context *, int);
extern const char *smb2_get_error(struct smb2_context *);
extern int smb2_connect_share(struct smb2_context *, const char *, const char *, const char *);
extern int smb2_disconnect_share(struct smb2_context *);
extern struct smb2dir *smb2_opendir(struct smb2_context *, const char *);
extern struct smb2dirent *smb2_readdir(struct smb2_context *, struct smb2dir *);
extern void smb2_closedir(struct smb2_context *, struct smb2dir *);
extern int smb2_stat(struct smb2_context *, const char *, struct smb2_stat_64 *);
extern struct smb2fh *smb2_open(struct smb2_context *, const char *, int);
extern int smb2_close(struct smb2_context *, struct smb2fh *);
extern int smb2_pread(struct smb2_context *, struct smb2fh *, uint8_t *, uint32_t, uint64_t);
extern uint32_t smb2_get_max_read_size(struct smb2_context *);

static NSString * const MXDSMBServerKey = @"madeira.directsmb.server.v1";
static NSString * const MXDSMBShareKey  = @"madeira.directsmb.share.v1";
static NSString * const MXDSMBPathKey   = @"madeira.directsmb.path.v1";
static NSString * const MXDSMBUserKey   = @"madeira.directsmb.user.v1";
static NSString * const MXDSMBDomainKey = @"madeira.directsmb.domain.v1";
static NSString * const MXDSMBExeKey    = @"madeira.directsmb.exe.v1";

static os_log_t mx_dsmb_log_handle(void)
{
    static os_log_t log;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ log = os_log_create("com.madeira.emulator", "direct-smb"); });
    return log;
}
static void mx_dsmb_log(NSString *tag, NSString *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    NSString *body = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    NSString *line = [NSString stringWithFormat:@"[%@] %@", tag, body ?: @""];
    os_log_with_type(mx_dsmb_log_handle(), OS_LOG_TYPE_DEFAULT, "%{public}s", line.UTF8String);
    dprintf(STDERR_FILENO, "%s\n", line.UTF8String);
}
static double mx_dsmb_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static NSString *mx_dsmb_normalize_remote(NSString *s)
{
    NSString *p = [s ?: @"" stringByReplacingOccurrencesOfString:@"\\" withString:@"/"];
    while ([p hasPrefix:@"/"]) p = [p substringFromIndex:1];
    while ([p hasSuffix:@"/"] && p.length) p = [p substringToIndex:p.length - 1];
    return p;
}
static NSString *mx_dsmb_join_remote(NSString *a, NSString *b)
{
    a = mx_dsmb_normalize_remote(a);
    b = mx_dsmb_normalize_remote(b);
    if (!a.length) return b ?: @"";
    if (!b.length) return a ?: @"";
    return [NSString stringWithFormat:@"%@/%@", a, b];
}
static NSString *mx_dsmb_windows_path(NSString *rel)
{
    return [rel ?: @"" stringByReplacingOccurrencesOfString:@"/" withString:@"\\"];
}
static uint64_t mx_dsmb_fnv64(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

@interface MXDirectSMB : NSObject
@property(nonatomic,strong) UIButton *button;
@property(nonatomic,copy) NSString *server;
@property(nonatomic,copy) NSString *share;
@property(nonatomic,copy) NSString *basePath;
@property(nonatomic,copy) NSString *user;
@property(nonatomic,copy) NSString *domain;
@property(nonatomic,copy) NSString *password;
@property(nonatomic,copy) NSString *selectedExe;
@property(nonatomic,strong) NSArray<NSString *> *executables;
@property(nonatomic,assign) struct smb2_context *ctx;
@property(nonatomic,assign) BOOL connected;
@property(nonatomic,assign) pthread_mutex_t smbLock;
@end

@implementation MXDirectSMB

+ (instancetype)shared
{
    static MXDirectSMB *x;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        x = [MXDirectSMB new];
        pthread_mutex_init(&x->_smbLock, NULL);
        NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
        x.server = [d stringForKey:MXDSMBServerKey] ?: @"192.168.129.129";
        x.share = [d stringForKey:MXDSMBShareKey] ?: @"";
        x.basePath = [d stringForKey:MXDSMBPathKey] ?: @"";
        x.user = [d stringForKey:MXDSMBUserKey] ?: @"";
        x.domain = [d stringForKey:MXDSMBDomainKey] ?: @"";
        x.selectedExe = [d stringForKey:MXDSMBExeKey];
        x.executables = @[];
    });
    return x;
}

- (NSString *)documentsPath { return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject; }
- (NSString *)cacheBase { return [[self documentsPath] stringByAppendingPathComponent:@"smb-vfs"]; }
- (NSString *)cacheRoot { return [[self cacheBase] stringByAppendingPathComponent:@"root"]; }
- (NSString *)metaRoot { return [[self cacheBase] stringByAppendingPathComponent:@"meta"]; }
- (NSString *)prefixPath { return [[self documentsPath] stringByAppendingPathComponent:@"wine"]; }

- (UIViewController *)topController
{
    UIWindow *key = nil;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (scene.activationState != UISceneActivationStateForegroundActive) continue;
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        UIWindowScene *ws = (UIWindowScene *)scene;
        for (UIWindow *w in ws.windows) if (w.isKeyWindow) { key = w; break; }
        if (!key) key = ws.windows.firstObject;
        if (key) break;
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (!key) key = UIApplication.sharedApplication.windows.firstObject;
#pragma clang diagnostic pop
    UIViewController *vc = key.rootViewController;
    while (vc.presentedViewController) vc = vc.presentedViewController;
    if ([vc isKindOfClass:UINavigationController.class]) vc = ((UINavigationController *)vc).topViewController;
    if ([vc isKindOfClass:UITabBarController.class]) vc = ((UITabBarController *)vc).selectedViewController;
    return vc;
}
- (UIWindow *)activeWindow
{
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (scene.activationState != UISceneActivationStateForegroundActive) continue;
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        UIWindowScene *ws = (UIWindowScene *)scene;
        for (UIWindow *w in ws.windows) if (w.isKeyWindow) return w;
        if (ws.windows.firstObject) return ws.windows.firstObject;
    }
    return nil;
}
- (void)alert:(NSString *)title message:(NSString *)message
{
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController *vc = [self topController];
        if (!vc) return;
        UIAlertController *a = [UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];
        [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [vc presentViewController:a animated:YES completion:nil];
    });
}
- (void)persist
{
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    [d setObject:self.server ?: @"" forKey:MXDSMBServerKey];
    [d setObject:self.share ?: @"" forKey:MXDSMBShareKey];
    [d setObject:self.basePath ?: @"" forKey:MXDSMBPathKey];
    [d setObject:self.user ?: @"" forKey:MXDSMBUserKey];
    [d setObject:self.domain ?: @"" forKey:MXDSMBDomainKey];
    if (self.selectedExe.length) [d setObject:self.selectedExe forKey:MXDSMBExeKey]; else [d removeObjectForKey:MXDSMBExeKey];
    [d synchronize];
}
- (void)disconnectLocked
{
    if (self.ctx) {
        if (self.connected) smb2_disconnect_share(self.ctx);
        smb2_destroy_context(self.ctx);
    }
    self.ctx = NULL;
    self.connected = NO;
}
- (int)connectLocked
{
    if (self.connected && self.ctx) return 0;
    [self disconnectLocked];
    if (!self.server.length || !self.share.length) return -EINVAL;
    struct smb2_context *ctx = smb2_init_context();
    if (!ctx) return -ENOMEM;
    self.ctx = ctx;
    smb2_set_timeout(ctx, 20);
    smb2_set_authentication(ctx, SMB2_SEC_NTLMSSP);
    smb2_set_user(ctx, self.user.UTF8String ?: "");
    smb2_set_password(ctx, self.password.UTF8String ?: "");
    if (self.domain.length) smb2_set_domain(ctx, self.domain.UTF8String);
    int rc = smb2_connect_share(ctx, self.server.UTF8String, self.share.UTF8String, self.user.UTF8String ?: "");
    if (rc < 0) {
        mx_dsmb_log(@"SMBDirect", @"connect FAILED server=%@ share=%@ rc=%d err=%s", self.server, self.share, rc, smb2_get_error(ctx) ?: "<none>");
        [self disconnectLocked];
        return rc;
    }
    self.connected = YES;
    mx_dsmb_log(@"SMBDirect", @"connected smb://%@/%@ base=%@", self.server, self.share, self.basePath ?: @"");
    return 0;
}

- (void)hideOldButtonsInView:(UIView *)view
{
    for (UIView *v in view.subviews) {
        if ([v isKindOfClass:UIButton.class]) {
            NSString *t = [(UIButton *)v titleForState:UIControlStateNormal] ?: @"";
            if ([t isEqualToString:@"JEU EXTERNE"] || [t isEqualToString:@"DOSSIER JEU"] || [t isEqualToString:@"SSD + EXE"] || [t isEqualToString:@"EXE NATIF"]) v.hidden = YES;
        }
        [self hideOldButtonsInView:v];
    }
}
- (void)installButton:(NSInteger)attempt
{
    dispatch_async(dispatch_get_main_queue(), ^{
        UIWindow *w = [self activeWindow];
        if (!w) {
            if (attempt < 100) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.35 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [self installButton:attempt + 1]; });
            return;
        }
        [self hideOldButtonsInView:w];
        if (!self.button.superview) {
            UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
            CGFloat width = 142.0;
            b.frame = CGRectMake(MAX(8.0, w.bounds.size.width - width - 10.0), w.safeAreaInsets.top + 8.0, width, 42.0);
            b.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleBottomMargin;
            [b setTitle:@"SMB DIRECT" forState:UIControlStateNormal];
            b.titleLabel.font = [UIFont boldSystemFontOfSize:11.5];
            b.backgroundColor = [UIColor colorWithRed:0.06 green:0.48 blue:0.26 alpha:0.97];
            [b setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
            b.layer.cornerRadius = 14.0;
            b.layer.borderWidth = 1.0;
            b.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.28].CGColor;
            [b addTarget:self action:@selector(showMenu) forControlEvents:UIControlEventTouchUpInside];
            [w addSubview:b]; [w bringSubviewToFront:b]; self.button = b;
            mx_dsmb_log(@"SMBDirect", @"direct SMB UI installed");
        } else [w bringSubviewToFront:self.button];
        if (attempt < 20) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [self installButton:attempt + 1]; });
    });
}
- (void)start { [self installButton:0]; [self ensureWineMappings]; }
- (void)afterMenu:(dispatch_block_t)block
{
    if (!block) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.20 * NSEC_PER_SEC)), dispatch_get_main_queue(), block);
}
- (void)showMenu
{
    UIViewController *vc = [self topController]; if (!vc) return;
    NSString *status = self.connected ? @"connecté" : @"déconnecté";
    NSString *target = self.share.length ? [NSString stringWithFormat:@"smb://%@/%@/%@", self.server, self.share, self.basePath ?: @""] : @"non configuré";
    NSString *msg = [NSString stringWithFormat:@"%@\nÉtat : %@\nLecteur Wine : E:\\\nEXE : %@\n\nAucune copie complète du jeu.", target, status, self.selectedExe ?: @"aucun"];
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"SMB direct / TrueNAS" message:msg preferredStyle:UIAlertControllerStyleActionSheet];
    __weak typeof(self) ws = self;
    [sheet addAction:[UIAlertAction actionWithTitle:@"Configurer le TrueNAS" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a){ [ws afterMenu:^{ [ws configure]; }]; }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Connecter + synchroniser les métadonnées" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a){ [ws afterMenu:^{ [ws syncMetadataAsync]; }]; }]];
    if (self.executables.count || self.selectedExe.length) [sheet addAction:[UIAlertAction actionWithTitle:@"Choisir le .exe" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a){ [ws afterMenu:^{ [ws chooseExe]; }]; }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Tester le SMB direct" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a){ [ws afterMenu:^{ [ws testAsync]; }]; }]];
    if (self.selectedExe.length) [sheet addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Lancer %@", self.selectedExe.lastPathComponent] style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a){ [ws afterMenu:^{ [ws launchSelected]; }]; }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Purger le cache SMB" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *a){ [ws afterMenu:^{ [ws purgeCache]; }]; }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Annuler" style:UIAlertActionStyleCancel handler:nil]];
    UIPopoverPresentationController *pop = sheet.popoverPresentationController;
    if (pop) { pop.sourceView = self.button ?: vc.view; pop.sourceRect = self.button ? self.button.bounds : CGRectMake(1,1,1,1); }
    [vc presentViewController:sheet animated:YES completion:nil];
}

- (void)configure
{
    UIViewController *vc = [self topController]; if (!vc) return;
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Connexion TrueNAS" message:@"Le mot de passe reste seulement en mémoire pour cette session." preferredStyle:UIAlertControllerStyleAlert];
    NSArray *place = @[@"Serveur / IP", @"Partage SMB", @"Chemin du jeu dans le partage", @"Utilisateur", @"Mot de passe", @"Domaine (optionnel)"];
    for (NSString *p in place) [a addTextFieldWithConfigurationHandler:^(UITextField *t){ t.placeholder = p; }];
    NSArray<UITextField *> *f = a.textFields;
    f[0].text = self.server.length ? self.server : @"192.168.129.129"; f[1].text = self.share; f[2].text = self.basePath; f[3].text = self.user; f[4].secureTextEntry = YES; f[4].textContentType = UITextContentTypePassword; f[5].text = self.domain;
    __weak typeof(self) ws = self;
    [a addAction:[UIAlertAction actionWithTitle:@"Annuler" style:UIAlertActionStyleCancel handler:nil]];
    [a addAction:[UIAlertAction actionWithTitle:@"Enregistrer" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *x){
        ws.server = f[0].text.length ? f[0].text : @"192.168.129.129"; ws.share = f[1].text ?: @""; ws.basePath = mx_dsmb_normalize_remote(f[2].text ?: @""); ws.user = f[3].text ?: @""; ws.password = f[4].text ?: @""; ws.domain = f[5].text ?: @"";
        pthread_mutex_lock(&ws->_smbLock); [ws disconnectLocked]; pthread_mutex_unlock(&ws->_smbLock); [ws persist];
        mx_dsmb_log(@"SMBDirect", @"configuration saved server=%@ share=%@ base=%@ user=%@", ws.server, ws.share, ws.basePath, ws.user);
        [ws syncMetadataAsync];
    }]];
    [vc presentViewController:a animated:YES completion:nil];
}

- (BOOL)ensureWineMappings
{
    NSFileManager *fm = NSFileManager.defaultManager; NSError *e = nil;
    [fm createDirectoryAtPath:self.cacheRoot withIntermediateDirectories:YES attributes:nil error:&e];
    [fm createDirectoryAtPath:self.metaRoot withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *prefix = self.prefixPath, *dos = [prefix stringByAppendingPathComponent:@"dosdevices"], *driveC = [prefix stringByAppendingPathComponent:@"drive_c"];
    [fm createDirectoryAtPath:dos withIntermediateDirectories:YES attributes:nil error:nil]; [fm createDirectoryAtPath:driveC withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *eLink = [dos stringByAppendingPathComponent:@"e:"], *compat = [driveC stringByAppendingPathComponent:@"__MadeiraDirectSMB"];
    for (NSString *link in @[eLink, compat]) {
        NSString *dst = [fm destinationOfSymbolicLinkAtPath:link error:nil];
        if (![dst isEqualToString:self.cacheRoot]) { [fm removeItemAtPath:link error:nil]; NSError *se = nil; [fm createSymbolicLinkAtPath:link withDestinationPath:self.cacheRoot error:&se]; if (se) mx_dsmb_log(@"WineDrive", @"symlink FAILED %@ -> %@ : %@", link, self.cacheRoot, se.localizedDescription); }
    }
    setenv("MADEIRA_DIRECT_SMB_CACHE_ROOT", self.cacheRoot.fileSystemRepresentation, 1);
    return !e;
}
- (NSString *)remoteForRelative:(NSString *)rel { return mx_dsmb_join_remote(self.basePath, rel); }
- (NSString *)relativeForLocalPath:(NSString *)path
{
    NSString *root = [self.cacheRoot stringByStandardizingPath], *p = [path stringByStandardizingPath];
    NSString *prefix = [root hasSuffix:@"/"] ? root : [root stringByAppendingString:@"/"];
    if (![p hasPrefix:prefix]) return nil; return [p substringFromIndex:prefix.length];
}
- (NSString *)fullMarkerForRelative:(NSString *)rel
{
    NSString *dir = [self.metaRoot stringByAppendingPathComponent:@"full"];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    uint64_t h = mx_dsmb_fnv64(rel.UTF8String); return [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%016llx.done", (unsigned long long)h]];
}
- (BOOL)isFullyMaterialized:(NSString *)rel { return [NSFileManager.defaultManager fileExistsAtPath:[self fullMarkerForRelative:rel]]; }
- (void)markFullyMaterialized:(NSString *)rel { [@"ok" writeToFile:[self fullMarkerForRelative:rel] atomically:YES encoding:NSUTF8StringEncoding error:nil]; }

- (int)prefetchRelative:(NSString *)rel offset:(uint64_t)offset length:(size_t)length localFD:(int)localFD full:(BOOL)full
{
    if (!rel.length) return -EINVAL; if (!full && [self isFullyMaterialized:rel]) return 0;
    NSString *remote = [self remoteForRelative:rel]; pthread_mutex_lock(&_smbLock); int rc = [self connectLocked]; if (rc < 0) { pthread_mutex_unlock(&_smbLock); return rc; }
    struct smb2_stat_64 st = {0}; rc = smb2_stat(self.ctx, remote.UTF8String, &st);
    if (rc < 0) { mx_dsmb_log(@"FileAccess", @"stat FAILED %@ rc=%d err=%s", remote, rc, smb2_get_error(self.ctx) ?: "<none>"); pthread_mutex_unlock(&_smbLock); return rc; }
    uint64_t fileSize = st.smb2_size; if (offset >= fileSize) { pthread_mutex_unlock(&_smbLock); return 0; }
    uint64_t end = full ? fileSize : MIN(fileSize, offset + (uint64_t)length); if (full) offset = 0;
    struct smb2fh *fh = smb2_open(self.ctx, remote.UTF8String, O_RDONLY); if (!fh) { mx_dsmb_log(@"FileAccess", @"open FAILED %@ err=%s", remote, smb2_get_error(self.ctx) ?: "<none>"); pthread_mutex_unlock(&_smbLock); return -EIO; }
    uint32_t maxRead = smb2_get_max_read_size(self.ctx); if (!maxRead || maxRead > 4*1024*1024) maxRead = 4*1024*1024; if (maxRead < 64*1024) maxRead = 64*1024;
    uint8_t *buf = malloc(maxRead); if (!buf) { smb2_close(self.ctx, fh); pthread_mutex_unlock(&_smbLock); return -ENOMEM; }
    double t0 = mx_dsmb_now(); uint64_t pos = offset; int out = 0;
    while (pos < end) { uint32_t want = (uint32_t)MIN((uint64_t)maxRead, end-pos); int got = smb2_pread(self.ctx, fh, buf, want, pos); if (got < 0) { out = got; break; } if (!got) break; ssize_t wr = pwrite(localFD, buf, (size_t)got, (off_t)pos); if (wr != got) { out = -(errno ?: EIO); break; } pos += (uint64_t)got; }
    free(buf); smb2_close(self.ctx, fh); pthread_mutex_unlock(&_smbLock);
    if (!out && full && pos >= fileSize) [self markFullyMaterialized:rel]; double dt = MAX(0.000001, mx_dsmb_now()-t0); double mib = (double)(pos-offset)/(1024.0*1024.0);
    mx_dsmb_log(@"WineVFS", @"%@ %@ bytes=%llu %.1f MiB/s", full ? @"materialize" : @"range", rel, (unsigned long long)(pos-offset), mib/dt); return out;
}
- (int)prefetchFD:(int)fd offset:(uint64_t)offset length:(size_t)length full:(BOOL)full
{
#ifdef F_GETPATH
    char pathbuf[PATH_MAX]; if (fcntl(fd, F_GETPATH, pathbuf) != 0) return 0;
    NSString *path = [NSString stringWithUTF8String:pathbuf], *rel = [self relativeForLocalPath:path]; if (!rel.length) return 0;
    return [self prefetchRelative:rel offset:offset length:length localFD:fd full:full];
#else
    return 0;
#endif
}

- (BOOL)syncDirectoryLocked:(NSString *)remoteRel localRel:(NSString *)localRel exes:(NSMutableArray<NSString *> *)exes depth:(NSInteger)depth
{
    if (depth > 64) return NO; NSString *remote = [self remoteForRelative:remoteRel]; struct smb2dir *dir = smb2_opendir(self.ctx, remote.UTF8String);
    if (!dir) { mx_dsmb_log(@"SMBDirect", @"opendir FAILED %@ err=%s", remote, smb2_get_error(self.ctx) ?: "<none>"); return NO; }
    NSString *localDir = localRel.length ? [self.cacheRoot stringByAppendingPathComponent:localRel] : self.cacheRoot; [NSFileManager.defaultManager createDirectoryAtPath:localDir withIntermediateDirectories:YES attributes:nil error:nil];
    struct smb2dirent *ent;
    while ((ent = smb2_readdir(self.ctx, dir))) {
        if (!ent->name || !strcmp(ent->name,".") || !strcmp(ent->name,"..")) continue; NSString *name = [NSString stringWithUTF8String:ent->name]; if (!name.length) continue;
        NSString *childRemoteRel = remoteRel.length ? [remoteRel stringByAppendingPathComponent:name] : name, *childLocalRel = localRel.length ? [localRel stringByAppendingPathComponent:name] : name, *local = [self.cacheRoot stringByAppendingPathComponent:childLocalRel];
        if (ent->st.smb2_type == SMB2_TYPE_DIRECTORY) { [NSFileManager.defaultManager createDirectoryAtPath:local withIntermediateDirectories:YES attributes:nil error:nil]; [self syncDirectoryLocked:childRemoteRel localRel:childLocalRel exes:exes depth:depth+1]; }
        else if (ent->st.smb2_type == SMB2_TYPE_FILE) { int fd = open(local.fileSystemRepresentation,O_CREAT|O_RDWR,0644); if (fd>=0) { ftruncate(fd,(off_t)ent->st.smb2_size); close(fd); } if ([[name.pathExtension lowercaseString] isEqualToString:@"exe"]) [exes addObject:childLocalRel]; }
    }
    smb2_closedir(self.ctx, dir); return YES;
}
- (void)syncMetadataAsync
{
    if (!self.share.length) { [self configure]; return; } [self ensureWineMappings];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
        NSMutableArray<NSString *> *exes = [NSMutableArray array]; pthread_mutex_lock(&self->_smbLock); int rc=[self connectLocked]; BOOL ok=NO; if (!rc) ok=[self syncDirectoryLocked:@"" localRel:@"" exes:exes depth:0]; pthread_mutex_unlock(&self->_smbLock);
        [exes sortUsingComparator:^NSComparisonResult(NSString *a, NSString *b){ BOOL al=[[a.lastPathComponent lowercaseString] isEqualToString:@"launcher.exe"], bl=[[b.lastPathComponent lowercaseString] isEqualToString:@"launcher.exe"]; if(al!=bl) return al?NSOrderedAscending:NSOrderedDescending; return [a localizedCaseInsensitiveCompare:b]; }];
        self.executables=exes; if(self.selectedExe.length && ![exes containsObject:self.selectedExe]) self.selectedExe=nil; if(!self.selectedExe.length && exes.count==1) self.selectedExe=exes.firstObject; [self persist];
        dispatch_async(dispatch_get_main_queue(), ^{ if(!ok) [self alert:@"SMB direct" message:@"Le scan SMB a échoué. Vérifie le partage, le chemin et les identifiants."]; else if(!exes.count) [self alert:@"SMB direct" message:@"Connexion OK, mais aucun .exe n'a été trouvé."]; else { mx_dsmb_log(@"SMBDirect", @"metadata sync complete: %lu EXE",(unsigned long)exes.count); [self chooseExe]; } });
    });
}
- (void)chooseExe
{
    if(!self.executables.count){ [self alert:@"EXE" message:@"Synchronise d'abord les métadonnées du dossier SMB."]; return; } UIViewController *vc=[self topController]; if(!vc)return;
    UIAlertController *a=[UIAlertController alertControllerWithTitle:@"Choisir l'exécutable" message:[NSString stringWithFormat:@"%lu fichier(s) .exe trouvé(s)",(unsigned long)self.executables.count] preferredStyle:UIAlertControllerStyleActionSheet]; __weak typeof(self) ws=self;
    NSUInteger limit=MIN((NSUInteger)80,self.executables.count); for(NSUInteger i=0;i<limit;i++){ NSString *rel=self.executables[i]; [a addAction:[UIAlertAction actionWithTitle:rel style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *x){ ws.selectedExe=rel; [ws persist]; mx_dsmb_log(@"SMBDirect",@"selected EXE %@",rel); }]]; }
    [a addAction:[UIAlertAction actionWithTitle:@"Annuler" style:UIAlertActionStyleCancel handler:nil]]; UIPopoverPresentationController *pop=a.popoverPresentationController; if(pop){pop.sourceView=self.button?:vc.view;pop.sourceRect=self.button?self.button.bounds:CGRectMake(1,1,1,1);}[vc presentViewController:a animated:YES completion:nil];
}
- (NSString *)findTestFileRelative
{
    if(self.selectedExe.length)return self.selectedExe; NSDirectoryEnumerator *en=[NSFileManager.defaultManager enumeratorAtPath:self.cacheRoot]; NSString *best=nil; unsigned long long bestSize=0;
    for(NSString *rel in en){ NSString *p=[self.cacheRoot stringByAppendingPathComponent:rel]; NSDictionary *attrs=[NSFileManager.defaultManager attributesOfItemAtPath:p error:nil]; if([attrs[NSFileType] isEqualToString:NSFileTypeRegular]){ unsigned long long n=[attrs fileSize]; if(n>bestSize){bestSize=n;best=rel;} } } return best;
}
- (void)testAsync
{
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
        NSString *rel=[self findTestFileRelative]; if(!rel.length){dispatch_async(dispatch_get_main_queue(),^{[self alert:@"Test SMB" message:@"Synchronise d'abord les métadonnées."];});return;} NSString *remote=[self remoteForRelative:rel]; double t0=mx_dsmb_now();
        pthread_mutex_lock(&self->_smbLock); int rc=[self connectLocked]; struct smb2_stat_64 st={0}; if(!rc)rc=smb2_stat(self.ctx,remote.UTF8String,&st); double latencyMs=(mx_dsmb_now()-t0)*1000.0, readStart=mx_dsmb_now(); uint64_t gotTotal=0;
        if(!rc && st.smb2_type==SMB2_TYPE_FILE){ struct smb2fh *fh=smb2_open(self.ctx,remote.UTF8String,O_RDONLY); if(fh){ uint32_t chunk=smb2_get_max_read_size(self.ctx); if(!chunk||chunk>4*1024*1024)chunk=4*1024*1024; uint8_t *buf=malloc(chunk); uint64_t wantTotal=MIN(st.smb2_size,(uint64_t)(16*1024*1024)); while(buf&&gotTotal<wantTotal){uint32_t want=(uint32_t)MIN((uint64_t)chunk,wantTotal-gotTotal);int n=smb2_pread(self.ctx,fh,buf,want,gotTotal);if(n<=0){if(n<0)rc=n;break;}gotTotal+=n;} free(buf);smb2_close(self.ctx,fh);}else rc=-EIO; }
        pthread_mutex_unlock(&self->_smbLock); double dt=MAX(0.000001,mx_dsmb_now()-readStart), speed=((double)gotTotal/(1024.0*1024.0))/dt;
        NSString *msg=[NSString stringWithFormat:@"Connexion : %@\nServeur : %@\nPartage : %@\nChemin : %@\nFichier test : %@\nTaille distante : %.2f Go\nLatence stat : %.1f ms\nLecture : %.2f Mo\nDébit : %.1f Mo/s\n\nCe test lit au maximum 16 Mo.",rc==0?@"OK":@"ÉCHEC",self.server,self.share,self.basePath?:@"",rel,(double)st.smb2_size/(1024.0*1024.0*1024.0),latencyMs,(double)gotTotal/(1024.0*1024.0),speed];
        mx_dsmb_log(@"SMBDirect",@"test rc=%d latency=%.1fms speed=%.1f MiB/s",rc,latencyMs,speed); dispatch_async(dispatch_get_main_queue(),^{[self alert:@"Test SMB direct" message:msg];});
    });
}
- (void)launchSelected
{
    if(!self.selectedExe.length){[self chooseExe];return;} if(wine_process_is_running()){[self alert:@"Wine" message:@"Ferme la session Wine actuelle avant de lancer le jeu SMB."];return;} [self ensureWineMappings];
    NSString *relWin=mx_dsmb_windows_path(self.selectedExe), *dirRel=[relWin stringByDeletingLastPathComponent]; if([dirRel isEqualToString:@"."])dirRel=@"";
    NSString *exe=[NSString stringWithFormat:@"C:\\__MadeiraDirectSMB\\%@",relWin], *dir=dirRel.length?[NSString stringWithFormat:@"C:\\__MadeiraDirectSMB\\%@",dirRel]:@"C:\\__MadeiraDirectSMB\\", *eDir=dirRel.length?[NSString stringWithFormat:@"E:\\%@",dirRel]:@"E:\\", *hostDir=[[self.cacheRoot stringByAppendingPathComponent:self.selectedExe] stringByDeletingLastPathComponent];
    setenv("MADEIRA_EXE",exe.UTF8String,1);setenv("MADEIRA_WORKDIR",dir.UTF8String,1);setenv("MADEIRA_INITIAL_CWD",eDir.UTF8String,1);setenv("MADEIRA_HOST_WORKDIR",hostDir.fileSystemRepresentation,1);setenv("MADEIRA_USE_ARM64EC","1",1);setenv("MADEIRA_DIRECT_SMB","1",1);setenv("MADEIRA_DIRECT_SMB_CACHE_ROOT",self.cacheRoot.fileSystemRepresentation,1);unsetenv("MADEIRA_ARGS");chdir(hostDir.fileSystemRepresentation);
    mx_dsmb_log(@"WineDrive",@"launch remote E:\\%@ actual=%@ host=%@",relWin,exe,hostDir); NSString *prefix=self.prefixPath; if(!wineserver_is_running()){int rc=wineserver_start(prefix.fileSystemRepresentation);if(rc){[self alert:@"Wine" message:[NSString stringWithFormat:@"wineserver_start=%d",rc]];return;}}
    __weak typeof(self) ws=self; __block int tries=0; __block void(^poll)(void); poll=^{if(wineserver_is_running()){int rc=wine_process_start(prefix.fileSystemRepresentation);mx_dsmb_log(@"WineDrive",@"wine_process_start=%d",rc);if(rc)[ws alert:@"Wine" message:[NSString stringWithFormat:@"wine_process_start=%d",rc]];return;}if(++tries>60){[ws alert:@"Wine" message:@"Le wineserver n'a pas démarré."];return;}dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(0.1*NSEC_PER_SEC)),dispatch_get_main_queue(),poll);};dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(0.1*NSEC_PER_SEC)),dispatch_get_main_queue(),poll);
}
- (void)purgeCache
{
    if(wine_process_is_running()){[self alert:@"Cache SMB" message:@"Ferme Wine avant de purger le cache."];return;} [NSFileManager.defaultManager removeItemAtPath:self.cacheBase error:nil];self.executables=@[];self.selectedExe=nil;[self persist];[self ensureWineMappings];mx_dsmb_log(@"SMBDirect",@"local sparse/cache tree purged; remote server untouched");[self alert:@"Cache SMB" message:@"Cache local supprimé. Aucun fichier n'a été supprimé sur le TrueNAS."];
}
@end

__attribute__((used, visibility("default"))) int madeira_direct_smb_prefetch_fd(int fd, unsigned long long offset, size_t length)
{ if(!getenv("MADEIRA_DIRECT_SMB"))return 0; return [[MXDirectSMB shared] prefetchFD:fd offset:(uint64_t)offset length:length full:NO]; }
__attribute__((used, visibility("default"))) int madeira_direct_smb_materialize_fd(int fd)
{ if(!getenv("MADEIRA_DIRECT_SMB"))return 0; return [[MXDirectSMB shared] prefetchFD:fd offset:0 length:0 full:YES]; }

__attribute__((constructor)) static void MXInstallDirectSMB(void)
{ dispatch_async(dispatch_get_main_queue(), ^{ [[MXDirectSMB shared] start]; }); }
