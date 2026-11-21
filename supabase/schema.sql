-- Run in the Supabase SQL Editor. Do not put the service_role key in firmware.
-- Create one Auth user per device, then register its UUID below.

create table if not exists public.device_registry (
    device_id text primary key,
    user_id uuid not null unique references auth.users(id) on delete cascade
);

create or replace function public.device_is_mine(target_device_id text)
returns boolean
language sql
stable
security definer
set search_path = public, auth
as $$
    select exists (
        select 1
        from public.device_registry r
        where r.device_id = target_device_id
          and r.user_id = auth.uid()
    );
$$;

revoke all on function public.device_is_mine(text) from public;
grant execute on function public.device_is_mine(text) to authenticated;

create table if not exists public.device_status (
    device_id text primary key,
    telemetry jsonb not null,
    updated_at timestamptz not null default now()
);

create or replace function public.touch_device_status_updated_at()
returns trigger
language plpgsql
set search_path = public
as $$
begin
    new.updated_at = now();
    return new;
end;
$$;

drop trigger if exists device_status_touch_updated_at on public.device_status;
create trigger device_status_touch_updated_at
before update on public.device_status
for each row execute function public.touch_device_status_updated_at();

create table if not exists public.telemetry_history (
    id bigint generated always as identity primary key,
    device_id text not null,
    event_id text not null,
    telemetry jsonb not null,
    inserted_at timestamptz not null default now(),
    unique (device_id, event_id)
);

create index if not exists telemetry_history_device_time_idx
on public.telemetry_history (device_id, inserted_at desc);

create table if not exists public.device_commands (
    command_id text primary key check (command_id ~ '^[A-Za-z0-9_-]{1,64}$'),
    device_id text not null,
    issued_at bigint not null,
    expires_at bigint not null,
    mode text not null check (mode in ('AUTO', 'OFF', 'MANUAL')),
    pump boolean,
    status text not null default 'pending'
        check (status in ('pending', 'accepted', 'rejected')),
    reason text,
    acknowledged_at bigint,
    created_at timestamptz not null default now(),
    check (expires_at > issued_at),
    check (mode <> 'MANUAL' or pump is not null)
);

create index if not exists device_commands_pending_idx
on public.device_commands (device_id, status, issued_at);

alter table public.device_registry enable row level security;
alter table public.device_status enable row level security;
alter table public.telemetry_history enable row level security;
alter table public.device_commands enable row level security;

revoke all on public.device_registry from anon, authenticated;
grant select, insert, update on public.device_status to authenticated;
grant insert on public.telemetry_history to authenticated;
grant select on public.device_commands to authenticated;
grant update (status, reason, acknowledged_at) on public.device_commands to authenticated;

drop policy if exists device_status_select_own on public.device_status;
create policy device_status_select_own on public.device_status
for select to authenticated using (public.device_is_mine(device_id));
drop policy if exists device_status_insert_own on public.device_status;
create policy device_status_insert_own on public.device_status
for insert to authenticated with check (public.device_is_mine(device_id));
drop policy if exists device_status_update_own on public.device_status;
create policy device_status_update_own on public.device_status
for update to authenticated
using (public.device_is_mine(device_id))
with check (public.device_is_mine(device_id));

drop policy if exists telemetry_insert_own on public.telemetry_history;
create policy telemetry_insert_own on public.telemetry_history
for insert to authenticated with check (public.device_is_mine(device_id));

drop policy if exists commands_select_own on public.device_commands;
create policy commands_select_own on public.device_commands
for select to authenticated using (public.device_is_mine(device_id));
drop policy if exists commands_ack_pending_own on public.device_commands;
create policy commands_ack_pending_own on public.device_commands
for update to authenticated
using (public.device_is_mine(device_id) and status = 'pending')
with check (public.device_is_mine(device_id) and status in ('accepted', 'rejected'));

-- Device accounts can read/update only their assigned rows. Insert commands
-- from a trusted server or the SQL Editor using the service_role context.
