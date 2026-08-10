#ifndef PPC_SPOTI_LOGIN_ITEMS_H
#define PPC_SPOTI_LOGIN_ITEMS_H

/*
 * Registers this app as a Login Item so it launches (visibly - a real
 * window, not a hidden service) when the user logs in, using
 * LSSharedFileListInsertItemURL against kLSSharedFileListSessionLoginItems
 * (Launch Services' Shared File List API, available since Mac OS X 10.5).
 *
 * Idempotent: checks the existing login items list first (via
 * LSSharedFileListCopySnapshot) so repeated launches never create
 * duplicate entries. Only meaningful when running from a real .app bundle
 * (needs [NSBundle mainBundle] to resolve to something real) - logs and
 * does nothing otherwise.
 */
void register_self_as_login_item(void);

#endif
