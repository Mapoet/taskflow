import type {Decision,ExecutionSnapshot,InteractionSnapshot,Manifest,Run,RuntimeEvent,SessionPage} from './types'
const headers=()=>({'Accept':'application/json','Content-Type':'application/json',
  'X-Agent-Tenant':localStorage.getItem('af.tenant')||'local',
  'X-Agent-Organization':localStorage.getItem('af.organization')||'local',
  'X-Agent-Project':localStorage.getItem('af.project')||'local',
  'X-Agent-Workspace':localStorage.getItem('af.workspace')||'local',
  'X-Agent-Principal':localStorage.getItem('af.principal')||'local-user'})
async function call<T>(path:string,init?:RequestInit):Promise<T>{const response=await fetch(path,{...init,headers:{...headers(),...(init?.headers||{})}});const body=await response.json().catch(()=>({}));if(!response.ok)throw Object.assign(new Error(body.error||`HTTP ${response.status}`),{status:response.status,body});return body as T}
export const api={
  sessions:(search='')=>call<SessionPage>(`/api/v1/sessions?limit=100&search=${encodeURIComponent(search)}`),
  capabilities:(session:string)=>call<Manifest>(`/api/v1/sessions/${encodeURIComponent(session)}/capabilities`),
  events:(session:string,after=0)=>call<{items:RuntimeEvent[];head:number;floor:number;next_cursor:number}>(`/api/v1/sessions/${encodeURIComponent(session)}/events?after=${after}&limit=200`),
  interactions:(session:string)=>call<InteractionSnapshot>(`/api/v1/sessions/${encodeURIComponent(session)}/interactions`),
  decision:(session:string,id:string)=>call<Decision>(`/api/v1/sessions/${encodeURIComponent(session)}/decisions/${encodeURIComponent(id)}`),
  run:(id:string)=>call<Run>(`/api/v1/runs/${encodeURIComponent(id)}`),
  snapshot:(session:string,task:string,run:string)=>call<ExecutionSnapshot>(`/api/v1/sessions/${encodeURIComponent(session)}/tasks/${encodeURIComponent(task)}/execution-snapshot?run_id=${encodeURIComponent(run)}`),
  answerDecision:(session:string,id:string,revision:number,option:string)=>call(`/api/v1/sessions/${encodeURIComponent(session)}/decisions/${encodeURIComponent(id)}/answer`,{method:'POST',body:JSON.stringify({expected_revision:revision,option_id:option})}),
  transitionSession:(session:string,revision:number,state:string)=>call(`/api/v1/sessions/${encodeURIComponent(session)}/state`,{method:'POST',body:JSON.stringify({expected_revision:revision,state})}),
  startRun:(session:string,input:string)=>{const run=`run-${crypto.randomUUID()}`;return call('/api/v1/runs',{method:'POST',body:JSON.stringify({session_id:session,run_id:run,command_id:`start-${run}`,provider_id:localStorage.getItem('af.provider')||'default',payload:{input}})})},
  command:(run:string,session:string,kind:string,revision:number,payload:Record<string,unknown>={})=>call(`/api/v1/runs/${encodeURIComponent(run)}/commands`,{method:'POST',body:JSON.stringify({session_id:session,command_id:`ui-${kind}-${crypto.randomUUID()}`,kind,expected_run_revision:revision,payload})})
}
