import type {Decision,ExecutionSnapshot,InteractionSnapshot,Manifest,Run,RuntimeEvent,RuntimeProfile,RuntimeSettings,SessionData,SessionPage} from './types'
const headers=()=>({'Accept':'application/json','Content-Type':'application/json',
  'X-Agent-Tenant':localStorage.getItem('af.tenant')||'local',
  'X-Agent-Organization':localStorage.getItem('af.organization')||'local',
  'X-Agent-Project':localStorage.getItem('af.project')||'local',
  'X-Agent-Workspace':localStorage.getItem('af.workspace')||'local',
  'X-Agent-Principal':localStorage.getItem('af.principal')||'local-user'})
async function call<T>(path:string,init?:RequestInit):Promise<T>{const response=await fetch(path,{...init,headers:{...headers(),...(init?.headers||{})}});const body=await response.json().catch(()=>({}));if(!response.ok)throw Object.assign(new Error(body.error||`HTTP ${response.status}`),{status:response.status,body});return body as T}
export const api={
  sessions:async(search='')=>{const page=await call<SessionPage>(`/api/v1/sessions?limit=100&state=all&search=${encodeURIComponent(search)}`);return{...page,items:page.items.filter((item)=>item.state!=='purged')}},
  profile:()=>call<RuntimeProfile>('/api/v1/me'),
  settings:()=>call<RuntimeSettings>('/api/v1/runtime/settings'),
  updateSettings:(revision:number,updates:Record<string,string|boolean>)=>call<{updated:boolean;revision:number;restart_required:boolean}>('/api/v1/runtime/settings',{method:'POST',body:JSON.stringify({expected_revision:revision,updates})}),
  createSession:(title:string,id:string)=>call('/api/v1/sessions',{method:'POST',body:JSON.stringify({session_id:id,conversation_id:`conversation-${crypto.randomUUID()}`,title,folder:'Local workspace',tags:[],pinned:false})}),
  renameSession:(session:string,revision:number,title:string)=>call(`/api/v1/sessions/${encodeURIComponent(session)}/title`,{method:'POST',body:JSON.stringify({expected_revision:revision,title})}),
  restoreSession:(session:string,revision:number)=>call(`/api/v1/sessions/${encodeURIComponent(session)}/restore`,{method:'POST',body:JSON.stringify({expected_revision:revision})}),
  purgeSession:async(session:string,revision:number,state:string)=>{let expected=revision;if(state==='trashed'){const pending=await call<{ok:boolean;revision:number}>(`/api/v1/sessions/${encodeURIComponent(session)}/purge`,{method:'POST',body:JSON.stringify({expected_revision:expected,confirm_permanent:false})});expected=pending.revision}return call(`/api/v1/sessions/${encodeURIComponent(session)}/purge`,{method:'POST',body:JSON.stringify({expected_revision:expected,confirm_permanent:true})})},
  capabilities:(session:string)=>call<Manifest>(`/api/v1/sessions/${encodeURIComponent(session)}/capabilities`),
  events:(session:string,after=0)=>call<{items:RuntimeEvent[];head:number;floor:number;next_cursor:number}>(`/api/v1/sessions/${encodeURIComponent(session)}/events?after=${after}&limit=200`),
  data:(session:string)=>call<SessionData>(`/api/v1/sessions/${encodeURIComponent(session)}/data?after=0&limit=200`),
  interactions:(session:string)=>call<InteractionSnapshot>(`/api/v1/sessions/${encodeURIComponent(session)}/interactions`),
  decision:(session:string,id:string)=>call<Decision>(`/api/v1/sessions/${encodeURIComponent(session)}/decisions/${encodeURIComponent(id)}`),
  run:(id:string)=>call<Run>(`/api/v1/runs/${encodeURIComponent(id)}`),
  snapshot:(session:string,task:string,run:string)=>call<ExecutionSnapshot>(`/api/v1/sessions/${encodeURIComponent(session)}/tasks/${encodeURIComponent(task)}/execution-snapshot?run_id=${encodeURIComponent(run)}`),
  answerDecision:(session:string,id:string,revision:number,option:string)=>call(`/api/v1/sessions/${encodeURIComponent(session)}/decisions/${encodeURIComponent(id)}/answer`,{method:'POST',body:JSON.stringify({expected_revision:revision,option_id:option})}),
  transitionSession:(session:string,revision:number,state:string)=>call(`/api/v1/sessions/${encodeURIComponent(session)}/state`,{method:'POST',body:JSON.stringify({expected_revision:revision,state})}),
  startRun:(session:string,input:string)=>{const run=`run-${crypto.randomUUID()}`;return call('/api/v1/runs',{method:'POST',body:JSON.stringify({session_id:session,run_id:run,command_id:`start-${run}`,provider_id:localStorage.getItem('af.provider')||'default',payload:{input}})})},
  command:(run:string,session:string,kind:string,revision:number,payload:Record<string,unknown>={})=>call(`/api/v1/runs/${encodeURIComponent(run)}/commands`,{method:'POST',body:JSON.stringify({session_id:session,command_id:`ui-${kind}-${crypto.randomUUID()}`,kind,expected_run_revision:revision,payload})})
}
