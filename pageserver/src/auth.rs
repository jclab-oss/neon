use utils::auth::{AuthError, Claims, Scope};
use utils::id::TenantId;

/// Checks that the claims grant full access to the given tenant, or to the management API if
/// `tenant_id` is `None`. Read-only tokens are rejected: use [`check_read_permission`] for the
/// requests that only read tenant data.
pub fn check_permission(claims: &Claims, tenant_id: Option<TenantId>) -> Result<(), AuthError> {
    match (&claims.scope, tenant_id) {
        (Scope::Tenant, None) => Err(AuthError(
            "Attempt to access management api with tenant scope. Permission denied".into(),
        )),
        (Scope::Tenant, Some(tenant_id)) => {
            if claims.tenant_id.unwrap() != tenant_id {
                return Err(AuthError("Tenant id mismatch. Permission denied".into()));
            }
            Ok(())
        }
        (Scope::PageServerApi, None) => Ok(()), // access to management api for PageServerApi scope
        (Scope::PageServerApi, Some(_)) => Ok(()), // access to tenant api using PageServerApi scope
        (Scope::TenantReadOnly, _) => Err(AuthError(
            "JWT scope 'TenantReadOnly' only permits reading tenant data. Permission denied".into(),
        )),
        (
            Scope::Admin
            | Scope::SafekeeperData
            | Scope::GenerationsApi
            | Scope::Infra
            | Scope::Scrubber
            | Scope::ControllerPeer
            | Scope::TenantEndpoint,
            _,
        ) => Err(AuthError(
            format!(
                "JWT scope '{:?}' is ineligible for Pageserver auth",
                claims.scope
            )
            .into(),
        )),
    }
}

/// Checks that the claims grant at least read access to the given tenant's data, e.g. for
/// GetPage and basebackup requests. This accepts everything that [`check_permission`] accepts,
/// plus [`Scope::TenantReadOnly`] tokens for the same tenant.
pub fn check_read_permission(claims: &Claims, tenant_id: TenantId) -> Result<(), AuthError> {
    match claims.scope {
        Scope::TenantReadOnly => {
            if claims.tenant_id != Some(tenant_id) {
                return Err(AuthError("Tenant id mismatch. Permission denied".into()));
            }
            Ok(())
        }
        _ => check_permission(claims, Some(tenant_id)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn claims(scope: Scope, tenant_id: Option<TenantId>) -> Claims {
        Claims::new(tenant_id, scope)
    }

    #[test]
    fn read_only_scope() {
        let tenant_id = TenantId::generate();
        let other_tenant_id = TenantId::generate();
        let read_only = claims(Scope::TenantReadOnly, Some(tenant_id));

        assert!(check_read_permission(&read_only, tenant_id).is_ok());
        assert!(check_read_permission(&read_only, other_tenant_id).is_err());
        assert!(check_read_permission(&claims(Scope::TenantReadOnly, None), tenant_id).is_err());

        // Anything that requires full access is denied.
        assert!(check_permission(&read_only, Some(tenant_id)).is_err());
        assert!(check_permission(&read_only, None).is_err());
    }

    #[test]
    fn read_permission_accepts_full_access_scopes() {
        let tenant_id = TenantId::generate();
        let other_tenant_id = TenantId::generate();

        let tenant = claims(Scope::Tenant, Some(tenant_id));
        assert!(check_read_permission(&tenant, tenant_id).is_ok());
        assert!(check_read_permission(&tenant, other_tenant_id).is_err());

        let pageserver_api = claims(Scope::PageServerApi, None);
        assert!(check_read_permission(&pageserver_api, tenant_id).is_ok());

        for scope in [
            Scope::Admin,
            Scope::SafekeeperData,
            Scope::GenerationsApi,
            Scope::Infra,
            Scope::Scrubber,
            Scope::ControllerPeer,
            Scope::TenantEndpoint,
        ] {
            assert!(check_read_permission(&claims(scope, Some(tenant_id)), tenant_id).is_err());
        }
    }
}
