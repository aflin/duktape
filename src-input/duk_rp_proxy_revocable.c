/*
 *  duk_rp_proxy_revocable.c
 *
 *  Install Proxy.revocable (ES2015).  Duktape ships Proxy support
 *  but not the .revocable static method.
 *
 *  Gated by DUK_RP_USE_PROXY_REVOCABLE.  Origin: rampart's
 *  src/duktape/register.c::install_proxy_revocable().
 */

#include "duk_internal.h"

#if defined(DUK_RP_USE_PROXY_REVOCABLE)

#include "duk_rp_internal.h"
#include <stdio.h>

static const char *duk__rp_proxy_revocable_js =
    "if(typeof Proxy==='function' && !Proxy.revocable){"
        "Proxy.revocable=function(target,handler){"
            "if(target==null||(typeof target!=='object'&&typeof target!=='function'))"
                "throw new TypeError('Cannot create proxy with a non-object as target');"
            "if(handler==null||typeof handler!=='object')"
                "throw new TypeError('Cannot create proxy with a non-object as handler');"
            "var revoked=false;"
            "var traps=['getPrototypeOf','setPrototypeOf','isExtensible','preventExtensions','getOwnPropertyDescriptor','defineProperty','has','get','set','deleteProperty','ownKeys','apply','construct'];"
            "var wrapped={};"
            "traps.forEach(function(t){"
                "wrapped[t]=function(){"
                    "if(revoked)throw new TypeError(\"Cannot perform '\"+t+\"' on a proxy that has been revoked\");"
                    "var fn=handler[t];"
                    "if(typeof fn==='function')return fn.apply(handler,arguments);"
                    "if(typeof Reflect!=='undefined'&&typeof Reflect[t]==='function')"
                        "return Reflect[t].apply(Reflect,arguments);"
                    "var a=arguments;"
                    "if(t==='get')return a[0][a[1]];"
                    "if(t==='set'){a[0][a[1]]=a[2];return true;}"
                    "if(t==='has')return a[1] in a[0];"
                    "if(t==='deleteProperty'){delete a[0][a[1]];return true;}"
                    "if(t==='ownKeys')return Object.getOwnPropertyNames(a[0]);"
                    "if(t==='getOwnPropertyDescriptor')return Object.getOwnPropertyDescriptor(a[0],a[1]);"
                    "throw new TypeError('Proxy trap \"'+t+'\" not available');"
                "};"
            "});"
            "return {proxy:new Proxy(target,wrapped),revoke:function(){revoked=true;}};"
        "};"
    "}";

DUK_INTERNAL void duk_rp_install_proxy_revocable(duk_context *ctx) {
    if (duk_peval_string(ctx, duk__rp_proxy_revocable_js) != 0) {
        fprintf(stderr, "duk_rp_install_proxy_revocable: %s\n", duk_safe_to_string(ctx, -1));
    }
    duk_pop(ctx);
}

#endif  /* DUK_RP_USE_PROXY_REVOCABLE */
