/* UIDocumentPicker is intentionally bypassed for network games.
   DirectSMBBridge uses UI action-handler blocks. Keep their controller object
   strong while those handlers are alive; ARC otherwise rejects taking the
   address of the C mutex through a __weak local. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-macro-redefinition"
#pragma push_macro("__weak")
#undef __weak
#define __weak
#include "DirectSMBBridge.inc"
#pragma pop_macro("__weak")
#pragma clang diagnostic pop
