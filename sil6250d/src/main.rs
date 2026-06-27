mod device;
mod engine;
mod storage;

use anyhow::Context;
use device::{DeviceService, OBJECT_PATH};
use futures_util::StreamExt;
use zbus::conn::Builder;

const MANAGER_DEST: &str = "net.reactivated.Fprint";
const MANAGER_PATH: &str = "/net/reactivated/Fprint/Manager";
const MANAGER_IFACE: &str = "net.reactivated.Fprint.Manager";
const SERVICE_NAME: &str = "io.github.uunicorn.Fprint";

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::from_default_env()
                .add_directive("sil6250d=info".parse().unwrap()),
        )
        .init();

    let devpath = std::env::var("SIL6250_DEV").unwrap_or_else(|_| "/dev/sil6250".into());
    tracing::info!(devpath, "starting sil6250d");

    let conn = Builder::system()?
        .name(SERVICE_NAME)?
        .serve_at(OBJECT_PATH, DeviceService::new(devpath))?
        .build()
        .await
        .context("connect to system D-Bus")?;

    register_with_manager(&conn).await?;

    tracing::info!("registered with open-fprintd manager; running");
    std::future::pending::<()>().await;
    Ok(())
}

async fn register_with_manager(conn: &zbus::Connection) -> anyhow::Result<()> {
    let proxy = zbus::fdo::DBusProxy::new(conn).await?;
    let mut name_owner_changed = proxy.receive_name_owner_changed().await?;

    if try_register(conn).await.is_ok() {
        return Ok(());
    }

    tracing::info!("open-fprintd manager not yet present; waiting for it to appear");

    tokio::spawn({
        let conn = conn.clone();
        async move {
            while let Some(signal) = name_owner_changed.next().await {
                if let Ok(args) = signal.args() {
                    if args.name() == MANAGER_DEST
                        && args.new_owner().as_deref().unwrap_or("") != ""
                    {
                        tracing::info!("open-fprintd appeared; registering");
                        if let Err(e) = try_register(&conn).await {
                            tracing::error!("register failed: {e}");
                        }
                    }
                }
            }
        }
    });

    Ok(())
}

async fn try_register(conn: &zbus::Connection) -> anyhow::Result<()> {
    let manager = zbus::Proxy::new(conn, MANAGER_DEST, MANAGER_PATH, MANAGER_IFACE)
        .await
        .context("create manager proxy")?;

    manager
        .call_method("RegisterDevice", &(OBJECT_PATH,))
        .await
        .context("RegisterDevice")?;

    tracing::info!("device registered at {OBJECT_PATH}");
    Ok(())
}
