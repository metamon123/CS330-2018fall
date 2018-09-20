Project 1  
Race issues :  
- 여러 thread 가 하나의 lock 에 대해서 동시에 lock\_acquire() 를 시도할 때, donation 여부를 결정하는 과정에서 race 로 심각하게 터져버릴 수 있음.  
  > sema\_donate 의 도입으로 해결  
- holder = NULL 인 lock 을 A thread 가 lock\_acquire() 에 성공하고 holder 를 A 로 세팅하기 직전에, 다른 B thread 가 lock\_acquire() 를 하면서  
  donation 여부를 결정하는 routine 에 들어서면 donation 을 해야하는 상황에도 donation 을 skip 할 수 있음.  
  > donation 여부를 결정할 때, sema\_try\_down() 으로 occupied 체크 후, holder 가 세팅 될 때까지 기다리고서 donation 여부를 결정하는 것으로 해결  
  > lock\_release 에서 holder = NULL 후, sema\_up() 을 하기 전에, 다른 thread 가 lock\_acquire()를 통해 donation 여부 결정 routine 으로 들어서면  
    dead lock 이 생길 수 있음. (thread 가 holder 하고 acquirer 2개밖에 없고, 위와 같은 상황이 발생하면 while 문에서 무한루프.  
  > 고민중... lock\_acquire() 에서 intr\_disable() 잘 켜서 하려고 했는데 잘 모르겠지만 데드락 걸림 ㅠㅠ
- holder 가 thread\_set\_priority() 를 실행하는 도중 다른 thread 가 lock\_acquire() 에서 donation 을 하는 경우 꼬일 수 있음.  
  > thread\_set\_priority() 에서도 sema\_donate 를 사용하는 것으로 해결  
  
