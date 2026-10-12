@import UIKit;
@import Foundation;
@import UniformTypeIdentifiers;
@import QuartzCore;
#include <stdlib.h>
#include <unistd.h>

extern int wineserver_start(const char *prefix_path);
extern int wineserver_is_running(void);
extern void wineserver_stop(void);
extern int wine_process_start(const char *prefix_path);
extern int wine_process_is_running(void);

@interface MXExternalLauncher : NSObject <UIDocumentPickerDelegate>
@property(nonatomic,strong) UIButton *button;
@property(nonatomic,strong) NSURL *scopedURL;
@property(nonatomic,assign) BOOL scopeStarted;
@end

@implementation MXExternalLauncher

+ (instancetype)shared {
    static MXExternalLauncher *obj;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ obj = [MXExternalLauncher new]; });
    return obj;
}

- (UIWindow *)activeWindow {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (scene.activationState != UISceneActivationStateForegroundActive) continue;
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        UIWindowScene *ws = (UIWindowScene *)scene;
        for (UIWindow *w in ws.windows) if (w.isKeyWindow) return w;
        if (ws.windows.firstObject) return ws.windows.firstObject;
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return UIApplication.sharedApplication.windows.firstObject;
#pragma clang diagnostic pop
}

- (UIViewController *)topController {
    UIViewController *vc = [self activeWindow].rootViewController;
    while (vc.presentedViewController) vc = vc.presentedViewController;
    if ([vc isKindOfClass:UINavigationController.class]) vc = ((UINavigationController *)vc).topViewController;
    if ([vc isKindOfClass:UITabBarController.class]) vc = ((UITabBarController *)vc).selectedViewController;
    return vc;
}

- (void)alert:(NSString *)title message:(NSString *)message {
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController *vc = [self topController];
        if (!vc) return;
        UIAlertController *a = [UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];
        [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [vc presentViewController:a animated:YES completion:nil];
    });
}

- (void)installButtonWhenReady:(NSInteger)attempt {
    dispatch_async(dispatch_get_main_queue(), ^{
        UIWindow *w = [self activeWindow];
        if (!w) {
            if (attempt < 30) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [self installButtonWhenReady:attempt + 1]; });
            return;
        }
        if (self.button.superview) return;
        UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
        b.frame = CGRectMake(MAX(12.0, w.bounds.size.width - 92.0), w.safeAreaInsets.top + 12.0, 78.0, 44.0);
        b.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleBottomMargin;
        [b setTitle:@"USB / SSD" forState:UIControlStateNormal];
        b.titleLabel.font = [UIFont boldSystemFontOfSize:12.0];
        b.backgroundColor = [UIColor colorWithWhite:0.08 alpha:0.86];
        [b setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
        b.layer.cornerRadius = 15.0;
        b.layer.borderWidth = 1.0;
        b.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.24].CGColor;
        b.accessibilityLabel = @"Lancer un jeu Windows depuis USB ou SSD";
        [b addTarget:self action:@selector(pickFolder) forControlEvents:UIControlEventTouchUpInside];
        [w addSubview:b];
        [w bringSubviewToFront:b];
        self.button = b;
    });
}

- (void)pickFolder {
    if (wine_process_is_running()) {
        [self alert:@"Madeira" message:@"Un jeu est déjà en cours. Ferme-le avant d'en lancer un autre depuis le stockage externe."];
        return;
    }
    UIViewController *vc = [self topController];
    if (!vc) return;
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder] asCopy:NO];
    picker.delegate = self;
    picker.allowsMultipleSelection = NO;
    [vc presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *root = urls.firstObject;
    if (!root) return;

    if (self.scopeStarted && self.scopedURL) {
        [self.scopedURL stopAccessingSecurityScopedResource];
        self.scopeStarted = NO;
    }

    self.scopedURL = root.URLByStandardizingPath;
    self.scopeStarted = [self.scopedURL startAccessingSecurityScopedResource];

    if (!self.scopeStarted) {
        [self alert:@"Accès au stockage" message:@"iOS n'a pas accordé l'accès permanent au dossier choisi. Rebranche le SSD/USB et sélectionne directement le dossier racine du jeu dans Fichiers."];
        return;
    }

    BOOL isDir = NO;
    if (![NSFileManager.defaultManager fileExistsAtPath:self.scopedURL.path isDirectory:&isDir] || !isDir) {
        [self alert:@"Dossier inaccessible" message:@"Le dossier sélectionné n'est plus accessible. Vérifie que le SSD/USB est toujours branché."];
        return;
    }

    [self chooseExecutableUnder:self.scopedURL];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {}

- (void)chooseExecutableUnder:(NSURL *)root {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSArray *keys = @[NSURLIsDirectoryKey, NSURLNameKey, NSURLIsReadableKey];
    NSDirectoryEnumerator<NSURL *> *en = [fm enumeratorAtURL:root
                                 includingPropertiesForKeys:keys
                                                    options:NSDirectoryEnumerationSkipsHiddenFiles
                                               errorHandler:^BOOL(NSURL *url, NSError *error) { return YES; }];
    NSMutableArray<NSURL *> *exes = [NSMutableArray array];
    NSUInteger rootDepth = root.pathComponents.count;

    for (NSURL *u in en) {
        NSNumber *isDir = nil;
        NSNumber *isReadable = nil;
        [u getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil];
        [u getResourceValue:&isReadable forKey:NSURLIsReadableKey error:nil];
        NSUInteger depth = u.pathComponents.count - rootDepth;

        if (isDir.boolValue && depth >= 10) {
            [en skipDescendants];
            continue;
        }
        if (!isDir.boolValue && isReadable.boolValue && [u.pathExtension.lowercaseString isEqualToString:@"exe"]) {
            [exes addObject:u.URLByStandardizingPath];
            if (exes.count >= 120) break;
        }
    }

    if (!exes.count) {
        [self alert:@"Aucun .exe" message:@"Aucun fichier .exe lisible n'a été trouvé dans le dossier du jeu. Sélectionne le dossier qui contient réellement le jeu Windows."];
        return;
    }

    NSString *rootPrefix = [root.path stringByAppendingString:@"/"];
    [exes sortUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) {
        NSString *ra = [a.path hasPrefix:rootPrefix] ? [a.path substringFromIndex:rootPrefix.length] : a.lastPathComponent;
        NSString *rb = [b.path hasPrefix:rootPrefix] ? [b.path substringFromIndex:rootPrefix.length] : b.lastPathComponent;
        return [ra localizedCaseInsensitiveCompare:rb];
    }];

    UIViewController *vc = [self topController];
    if (!vc) return;

    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"Choisir le .exe"
                                                                    message:@"Tout le dossier sélectionné restera sur le SSD/USB et sera monté dans Wine sous C:\\__ExternalGame."
                                                             preferredStyle:UIAlertControllerStyleActionSheet];

    for (NSURL *exe in exes) {
        NSString *relative = [exe.path hasPrefix:rootPrefix] ? [exe.path substringFromIndex:rootPrefix.length] : exe.lastPathComponent;
        [sheet addAction:[UIAlertAction actionWithTitle:relative style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
            [self launchExecutable:exe fromRoot:root];
        }]];
    }

    [sheet addAction:[UIAlertAction actionWithTitle:@"Annuler" style:UIAlertActionStyleCancel handler:nil]];
    UIPopoverPresentationController *pop = sheet.popoverPresentationController;
    if (pop) {
        pop.sourceView = self.button ?: vc.view;
        pop.sourceRect = self.button ? self.button.bounds : CGRectMake(vc.view.bounds.size.width - 1, 1, 1, 1);
    }
    [vc presentViewController:sheet animated:YES completion:nil];
}

- (void)launchExecutable:(NSURL *)exe fromRoot:(NSURL *)root {
    if (!self.scopeStarted || !self.scopedURL) {
        [self alert:@"Accès perdu" message:@"L'accès au dossier du jeu n'est plus actif. Sélectionne de nouveau le dossier USB/SSD."];
        return;
    }

    NSString *rootPath = root.URLByStandardizingPath.path;
    NSString *exePath = exe.URLByStandardizingPath.path;
    NSString *rootPrefix = [rootPath stringByAppendingString:@"/"];

    if (![exePath hasPrefix:rootPrefix]) {
        [self alert:@"Chemin refusé" message:@"Le .exe doit se trouver à l'intérieur du dossier de jeu sélectionné."];
        return;
    }

    NSString *relativeExe = [exePath substringFromIndex:rootPrefix.length];
    if (!relativeExe.length || [relativeExe containsString:@"../"] || [relativeExe hasPrefix:@"/"]) {
        [self alert:@"Chemin invalide" message:@"Le chemin du .exe n'est pas valide."];
        return;
    }

    if (![relativeExe canBeConvertedToEncoding:NSUTF8StringEncoding]) {
        [self alert:@"Nom de fichier" message:@"Le chemin du jeu contient des caractères non pris en charge."];
        return;
    }

    BOOL readable = [NSFileManager.defaultManager isReadableFileAtPath:exePath];
    if (!readable) {
        [self alert:@"EXE inaccessible" message:@"Le .exe n'est pas lisible. Vérifie que le SSD/USB est toujours branché."];
        return;
    }

    NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    if (!documents.length) {
        [self alert:@"Erreur" message:@"Dossier Documents introuvable."];
        return;
    }

    NSString *prefix = [documents stringByAppendingPathComponent:@"wine"];
    NSString *driveC = [prefix stringByAppendingPathComponent:@"drive_c"];
    NSString *mount = [driveC stringByAppendingPathComponent:@"__ExternalGame"];
    NSFileManager *fm = NSFileManager.defaultManager;
    NSError *err = nil;

    [fm createDirectoryAtPath:driveC withIntermediateDirectories:YES attributes:nil error:&err];
    if (err) {
        [self alert:@"Erreur Wine" message:err.localizedDescription];
        return;
    }

    [fm removeItemAtPath:mount error:nil];
    err = nil;
    if (![fm createSymbolicLinkAtPath:mount withDestinationPath:rootPath error:&err]) {
        [self alert:@"Montage USB/SSD impossible" message:err.localizedDescription ?: @"Impossible de monter le dossier externe dans Wine."];
        return;
    }

    NSString *mountedProbe = [mount stringByAppendingPathComponent:relativeExe];
    if (![fm isReadableFileAtPath:mountedProbe]) {
        [fm removeItemAtPath:mount error:nil];
        [self alert:@"Montage inaccessible" message:@"Le lien vers le stockage externe a été créé mais Madeira ne peut pas lire le .exe à travers ce montage. Rebranche le SSD/USB puis réessaie."];
        return;
    }

    NSString *winRelativeExe = [relativeExe stringByReplacingOccurrencesOfString:@"/" withString:@"\\"];
    NSString *relativeDir = [relativeExe stringByDeletingLastPathComponent];
    NSString *winRelativeDir = [relativeDir stringByReplacingOccurrencesOfString:@"/" withString:@"\\"];

    NSString *winExe = [NSString stringWithFormat:@"C:\\__ExternalGame\\%@", winRelativeExe];
    NSString *winWorkdir = winRelativeDir.length
        ? [NSString stringWithFormat:@"C:\\__ExternalGame\\%@", winRelativeDir]
        : @"C:\\__ExternalGame";

    setenv("MADEIRA_EXE", winExe.UTF8String, 1);
    setenv("MADEIRA_WORKDIR", winWorkdir.UTF8String, 1);
    setenv("MADEIRA_EXTERNAL_ROOT", rootPath.UTF8String, 1);
    unsetenv("MADEIRA_ARGS");

    if (wine_process_is_running()) {
        [self alert:@"Madeira" message:@"Un jeu est déjà en cours."];
        return;
    }

    int ws = wineserver_is_running();
    if (!ws) {
        int rc = wineserver_start(prefix.fileSystemRepresentation);
        if (rc != 0) {
            [self alert:@"wineserver" message:[NSString stringWithFormat:@"Impossible de démarrer wineserver (code %d).", rc]];
            return;
        }
    }

    __block NSInteger tries = 0;
    __weak typeof(self) weakSelf = self;
    __block void (^poll)(void) = nil;
    poll = ^{
        tries++;
        if (wineserver_is_running()) {
            int rc = wine_process_start(prefix.fileSystemRepresentation);
            if (rc != 0) {
                [weakSelf alert:@"Lancement impossible" message:[NSString stringWithFormat:@"wine_process_start a renvoyé %d.", rc]];
            } else {
                [weakSelf alert:@"Jeu lancé" message:[NSString stringWithFormat:@"%@\n\nDossier complet monté : C:\\__ExternalGame\nDossier de travail : %@", relativeExe, winWorkdir]];
            }
            poll = nil;
            return;
        }
        if (tries >= 100) {
            [weakSelf alert:@"wineserver" message:@"wineserver ne s'est pas déclaré prêt."];
            poll = nil;
            return;
        }
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.10 * NSEC_PER_SEC)), dispatch_get_main_queue(), poll);
    };
    dispatch_async(dispatch_get_main_queue(), poll);
}
@end

__attribute__((constructor))
static void MXInstallExternalStorageOverlay(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [[MXExternalLauncher shared] installButtonWhenReady:0];
    });
}
