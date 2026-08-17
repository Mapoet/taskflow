import {useEffect,useState} from 'react'
import {useMutation,useQuery,useQueryClient} from '@tanstack/react-query'
import {api} from './api'
import type {Capability,Run,RuntimeEvent,Session} from './types'

const glyph:Record<string,string>={understanding:'◈',decision:'?',plan:'⌁',plan_node:'·',tool_invocation:'⚙',agent:'◎',memory_view:'◇',approval:'!',evidence:'✓',artifact:'▣',closure:'◆'}
function SessionRail({items,active,search,onSearch,onSelect}:{items:Session[];active?:string;search:string;onSearch:(value:string)=>void;onSelect:(id:string)=>void}) {
  return <aside className="rail"><div className="brand"><span className="brand-mark">A</span><div><strong>Agent</strong><small>Workbench</small></div></div><label className="search"><span>⌕</span><input aria-label="Search sessions" placeholder="Search sessions" value={search} onChange={e=>onSearch(e.target.value)}/></label><div className="section-label">Sessions <span>{items.length}</span></div><nav>{items.map(item=><button key={item.session_id} className={active===item.session_id?'session active':'session'} onClick={()=>onSelect(item.session_id)}><span className="session-dot"/><span><strong>{item.title||item.session_id}</strong><small>{item.state} · r{item.revision}</small></span></button>)}</nav><div className="rail-foot"><span className="status-dot"/> Runtime connected</div></aside>
}

function RunStrip({events,actions,run,onCancel,cancelling}:{events:RuntimeEvent[];actions:Capability[];run?:Run;onCancel:()=>void;cancelling:boolean}) {
  const latest=events.at(-1),action=actions.find(a=>a.action_id==='run.cancel')
  const terminal=!run||['completed','failed','cancelled'].includes(run.state)
  return <section className="run-strip" aria-label="Run status"><div><span className={terminal?'status-dot':'pulse'}/><strong>{run?`Run ${run.state}`:'No active run'}</strong><small>{run?.run_id||latest?.run_id||'Select or start a Session'}</small></div><div className="run-meta"><span>Event head <b>{latest?.sequence||0}</b></span><span>Command cursor <b>{run?.command_cursor||0}</b></span><span>Run revision <b>{run?.revision||0}</b></span></div><div className="run-actions"><button disabled={terminal||!action?.enabled||cancelling} title={action?.reason||'Cancel this run'} onClick={onCancel}>cancel</button></div></section>
}

function DecisionCard({session,id,canAnswer=true}:{session:string;id:string;canAnswer?:boolean}) {
  const q=useQuery({queryKey:['decision',session,id],queryFn:()=>api.decision(session,id)})
  const client=useQueryClient(),answer=useMutation({mutationFn:(option:string)=>api.answerDecision(session,id,q.data!.revision,option),onSuccess:()=>client.invalidateQueries({queryKey:['decision',session,id]})})
  if(q.isLoading)return <div className="card skeleton">Loading decision…</div>
  if(q.error||!q.data)return null
  return <article className="card decision"><header><span>Decision required</span><em>r{q.data.revision}</em></header><h3>{q.data.question}</h3><div className="choices">{q.data.options.map(o=><button key={o.id} disabled={!canAnswer||q.data!.state!=='pending'||answer.isPending} title={canAnswer?'Answer this revision':'Capability manifest denies decision.answer'} onClick={()=>answer.mutate(o.id)}><strong>{o.label}</strong><small>{o.description}</small></button>)}</div>{answer.error&&<p className="error">{answer.error.message}</p>}</article>
}

function Workbench({session}:{session?:Session}) {
  const capabilities=useQuery({queryKey:['capabilities',session?.session_id],queryFn:()=>api.capabilities(session!.session_id),enabled:!!session,refetchInterval:5000})
  const events=useQuery({queryKey:['events',session?.session_id],queryFn:()=>api.events(session!.session_id),enabled:!!session,refetchInterval:1500})
  const interactions=useQuery({queryKey:['interactions',session?.session_id],queryFn:()=>api.interactions(session!.session_id),enabled:!!session,refetchInterval:1500})
  const nodes=interactions.data?.nodes||[]
  const latestRunId=[...(events.data?.items||[])].reverse().find(e=>e.run_id)?.run_id||''
  const latestTaskId=[...(events.data?.items||[])].reverse().map(e=>String(e.payload.task_id||'')).find(Boolean)||''
  const run=useQuery({queryKey:['run',latestRunId],queryFn:()=>api.run(latestRunId),enabled:!!latestRunId,refetchInterval:1500})
  const snapshot=useQuery({queryKey:['snapshot',session?.session_id,latestTaskId,latestRunId],queryFn:()=>api.snapshot(session!.session_id,latestTaskId,latestRunId),enabled:!!session&&!!latestTaskId&&!!latestRunId,refetchInterval:1500})
  const decisionIds=[...new Set(nodes.filter(n=>n.kind==='decision').map(n=>String(n.ref.decision_id||'')).filter(Boolean))]
  const [drawer,setDrawer]=useState('activity'),[input,setInput]=useState('')
  const client=useQueryClient()
  const runIsActive=!!run.data&&!['completed','failed','cancelled'].includes(run.data.state)
  const submit=useMutation({mutationFn:()=>runIsActive
    ?api.command(run.data!.run_id,session!.session_id,'steer',run.data!.revision,{input})
    :api.startRun(session!.session_id,input),onSuccess:()=>{setInput('');client.invalidateQueries({queryKey:['events',session?.session_id]});client.invalidateQueries({queryKey:['run',latestRunId]})}})
  const cancel=useMutation({mutationFn:()=>api.command(run.data!.run_id,session!.session_id,'cancel',run.data!.revision),onSuccess:()=>{client.invalidateQueries({queryKey:['run',latestRunId]});client.invalidateQueries({queryKey:['events',session?.session_id]})}})
  const archive=useMutation({mutationFn:()=>api.transitionSession(session!.session_id,
    capabilities.data?.session_revision??session!.revision,'archived'),onSuccess:()=>client.invalidateQueries({queryKey:['sessions']})})
  if(!session)return <main className="empty"><div className="empty-orbit">A</div><h1>Select a Session</h1><p>Conversation, planning, execution and evidence share one authoritative event cursor.</p></main>
  const startAction=capabilities.data?.actions.find(a=>a.action_id==='run.start')
  const steerAction=capabilities.data?.actions.find(a=>a.action_id==='run.steer')
  const decisionAction=capabilities.data?.actions.find(a=>a.action_id==='decision.answer')
  const sendEnabled=runIsActive?steerAction?.enabled:startAction?.enabled
  const drawerKinds:Record<string,string[]>= {activity:[],understanding:['understanding'],plan:['plan','plan_node'],memory:['memory_view'],files:['artifact'],approval:['approval'],evidence:['evidence','closure']}
  const visibleNodes=drawer==='activity'?nodes:nodes.filter(n=>drawerKinds[drawer]?.includes(n.kind))
  const commandError=submit.error||cancel.error
  const transitionAction=capabilities.data?.actions.find(a=>a.action_id==='session.transition')
  return <main className="workspace"><header className="topbar"><div><small>{session.folder||'Workspace'} / Session</small><h1>{session.title||session.session_id}</h1></div><div className="top-actions"><button disabled={!transitionAction?.enabled||archive.isPending||runIsActive} title={runIsActive?'Cancel or complete the active Run before archiving':transitionAction?.reason||'Archive Session'} onClick={()=>archive.mutate()}>Archive</button><div className="revision">Session r{capabilities.data?.session_revision??session.revision}<span>{snapshot.data?`Task r${snapshot.data.task_revision} · Run r${snapshot.data.run_revision} · Projection r${snapshot.data.projection_revision}`:'authoritative'}</span></div></div></header><RunStrip events={events.data?.items||[]} actions={capabilities.data?.actions||[]} run={run.data} onCancel={()=>cancel.mutate()} cancelling={cancel.isPending}/><div className="work-grid"><section className="conversation"><div className="context-tabs"><button className="active">Conversation</button><button onClick={()=>setDrawer('understanding')}>Understanding</button><button onClick={()=>setDrawer('plan')}>Plan</button></div><div className="timeline">{events.isError&&<div className="notice error"><strong>Event stream unavailable</strong><span>{events.error.message}</span></div>}{interactions.data?.stale&&<div className="notice error"><strong>Observation projection is catching up</strong><span>Event head {interactions.data.runtime_event_head}; projection head {interactions.data.head_sequence}.</span></div>}{commandError&&<div className="notice error"><strong>Run command conflict</strong><span>{commandError.message}; refresh uses the authoritative run revision.</span></div>}{archive.error&&<div className="notice error"><strong>Session transition failed</strong><span>{archive.error.message}</span></div>}{nodes.length===0&&!interactions.isLoading&&<div className="welcome"><span>◈</span><h2>Ready for a governed task</h2><p>Semantic decisions, plans, tool observations and closure evidence appear here as durable facts.</p></div>}{decisionIds.map(id=><DecisionCard key={id} session={session.session_id} id={id} canAnswer={!!decisionAction?.enabled}/>)}{nodes.slice(-20).map(node=><article className={`event ${node.kind}`} key={node.node_id}><span className="event-icon">{glyph[node.kind]||'·'}</span><div><header><strong>{node.label}</strong><time>{node.state}</time></header><p>{node.summary||`Durable event from run ${String(node.ref.run_id||'')}`}</p></div></article>)}</div><form className="composer" onSubmit={e=>{e.preventDefault();if(input.trim()&&sendEnabled)submit.mutate()}}><textarea aria-label="Message" value={input} onChange={e=>setInput(e.target.value)} placeholder={runIsActive?'Add information or steer the active run…':'Describe a governed task…'}/><footer><span>{runIsActive?`Steer will require Run r${run.data!.revision}`:'Start a new governed Run'}</span><button disabled={!sendEnabled||!input.trim()||submit.isPending} title={(runIsActive?steerAction:startAction)?.reason||'Submit a governed command'}>{runIsActive?'Steer':'Start'}</button></footer></form></section><aside className="drawer"><nav>{['activity','understanding','plan','memory','files','approval','evidence'].map(x=><button key={x} className={drawer===x?'active':''} onClick={()=>setDrawer(x)}>{x}</button>)}</nav><div className="drawer-body"><h2>{drawer[0].toUpperCase()+drawer.slice(1)}</h2><p className="muted">Event head {interactions.data?.runtime_event_head||events.data?.head||0} · Projection head {interactions.data?.head_sequence||0}</p>{visibleNodes.slice(-12).map(n=><div className="mini" key={n.node_id}><span>{glyph[n.kind]||'·'}</span><div><strong>{n.label}</strong><small>{n.state}</small></div></div>)}{visibleNodes.length===0&&<p className="muted">No durable {drawer} facts have been published for this Session.</p>}</div></aside></div></main>
}

export function App() {
  const [search,setSearch]=useState(''),[selected,setSelected]=useState(()=>location.hash.slice(1))
  const sessions=useQuery({queryKey:['sessions',search],queryFn:()=>api.sessions(search),refetchInterval:10000})
  useEffect(()=>{if(sessions.data&&!sessions.data.items.some(item=>item.session_id===selected))setSelected(sessions.data.items[0]?.session_id||'')},[sessions.data,selected])
  useEffect(()=>{if(selected)history.replaceState(null,'',`#${selected}`)},[selected])
  const active=sessions.data?.items.find(s=>s.session_id===selected)
  return <div className="app"><SessionRail items={sessions.data?.items||[]} active={selected} search={search} onSearch={setSearch} onSelect={setSelected}/>{sessions.isError?<main className="empty"><div className="notice error"><strong>Cannot load Sessions</strong><span>{sessions.error.message}</span></div><h1>Runtime connection required</h1><p>Configure the authenticated `/api/v1` endpoint, then retry.</p><button onClick={()=>sessions.refetch()}>Retry</button></main>:<Workbench session={active}/>}<div className="mobile-switcher"><button onClick={()=>document.querySelector('.rail')?.classList.toggle('open')}>☰ Sessions</button><span>{active?.title||'Agent Workbench'}</span></div></div>
}
