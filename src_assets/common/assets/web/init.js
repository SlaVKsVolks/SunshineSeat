import i18n from './locale'
import { getSessionState, installApiAuthRedirect } from './fetch_utils'

// must import even if not implicitly using here
// https://github.com/aurelia/skeleton-navigation/issues/894
// https://discourse.aurelia.io/t/bootstrap-import-bootstrap-breaks-dropdown-menu-in-navbar/641/9
import 'bootstrap/dist/js/bootstrap'

export async function initApp(app, config, options = {}) {
    const {
        requireAuth = true,
        redirectAuthenticatedTo = null,
    } = options;

    installApiAuthRedirect();

    if (requireAuth || redirectAuthenticatedTo) {
        const session = await getSessionState();
        if (requireAuth && !session.authenticated) {
            window.location.assign('/login');
            return;
        }
        if (redirectAuthenticatedTo && session.authenticated) {
            window.location.assign(redirectAuthenticatedTo);
            return;
        }
        app.provide('session', session);
    }

    //Wait for locale initialization, then render
    const initializedI18n = await i18n();
    app.use(initializedI18n);
    app.provide('i18n', initializedI18n.global)
    app.mount('#app');
    if (config) {
        config(app)
    }
}
